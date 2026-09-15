/**
 * @file synth_app.cpp
 * @brief FM synth app logic: banks, voice selection, macro mapping, random voices, console verbs.
 *
 * Macro dials shape the current voice without exposing operator parameters:
 *  - BRIGHT  scales the output level of every modulator operator (carriers untouched),
 *  - ATTACK  moves the carriers' first envelope rate,
 *  - RELEASE moves every operator's last envelope rate,
 *  - MOTION  adds LFO pitch and amplitude modulation and speeds the LFO up.
 * Each dial's centre (50) means "as the voice was designed"; the edited voice is applied live.
 */
#include "synth_app.h"
#include "synth_engine.h"
#include "synth_ui.h"
#include "console.h"
#include "stream.h"
#include "launcher.h"
#include "settings.h"
#include "theme.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "synth_app";

namespace {

// ------------------------------------------------------------------ voice layout (unpacked, 155 bytes)
// Per operator (6 × 21 bytes, operator 6 first): R1 R2 R3 R4 L1 L2 L3 L4 BP LD RD LC RC RS AMS KVS OL MODE FC FF DET
// Global from 126: PR1-4 PL1-4 ALG FB OKS LFS LFD LPMD LAMD LFKS LFW LPMS TRNSP NAME[10]
constexpr int OP_R1 = 0, OP_R4 = 3, OP_OL = 16, G_ALG = 134, G_LFS = 137, G_LPMD = 139, G_LAMD = 140, G_NAME = 145;
constexpr int OP_BYTES = 21;

/** Carrier operators (1-based) per DX7 algorithm, as a bit mask over operators 1..6. */
constexpr uint8_t CARRIERS[32] = {
    0b000101, 0b000101, 0b001001, 0b001001, 0b010101, 0b010101, 0b000101, 0b000101,
    0b000101, 0b001001, 0b001001, 0b000101, 0b000101, 0b000101, 0b000101, 0b000001,
    0b000001, 0b000001, 0b011001, 0b001011, 0b011011, 0b011101, 0b011011, 0b011111,
    0b011111, 0b001011, 0b001011, 0b100101, 0b010111, 0b100111, 0b011111, 0b111111
};
bool is_carrier(int alg1, int op1) { return (CARRIERS[(alg1 - 1) & 31] >> (op1 - 1)) & 1; }
/** Byte offset of operator op1 (1..6): the data stores operator 6 first. */
int op_off(int op1) { return (6 - op1) * OP_BYTES; }
int clamp99(int v) { return v < 0 ? 0 : v > 99 ? 99 : v; }

uint8_t base_voice[synth::VOICE_PARAMS];   ///< the voice as loaded; macros derive from it
int macro[4] = {50, 50, 50, 50};
int voice_idx = 0;
int octave_base = 48;
uint8_t sd_bank[4096]; bool have_sd_bank = false;

/** Apply the macro dials to a copy of the base voice and push it into the engine. */
void apply_macros()
{
    uint8_t v[synth::VOICE_PARAMS];
    memcpy(v, base_voice, sizeof v);
    const int alg = v[G_ALG] + 1;
    const float bright = powf(2.0f, (macro[synth_ui::MACRO_BRIGHT] - 50) / 25.0f);   // ×0.25 .. ×4
    const int attack = (50 - macro[synth_ui::MACRO_ATTACK]) * 4 / 5;                    // -40 .. +40 on R1
    const int release = (50 - macro[synth_ui::MACRO_RELEASE]) * 4 / 5;                  // -40 .. +40 on R4
    const int motion = macro[synth_ui::MACRO_MOTION] - 50;                              // -50 .. +50
    for (int op = 1; op <= 6; op++) {
        uint8_t *o = v + op_off(op);
        if (!is_carrier(alg, op)) o[OP_OL] = clamp99((int)(o[OP_OL] * bright));
        else o[OP_R1] = clamp99(o[OP_R1] + attack);
        o[OP_R4] = clamp99(o[OP_R4] + release);
    }
    if (motion > 0) {
        v[G_LPMD] = clamp99(v[G_LPMD] + motion / 2);
        v[G_LAMD] = clamp99(v[G_LAMD] + motion / 3);
        v[G_LFS]  = clamp99(v[G_LFS] + motion / 4);
    } else {
        v[G_LPMD] = clamp99(v[G_LPMD] + motion);     // turning left removes designed-in vibrato
        v[G_LAMD] = clamp99(v[G_LAMD] + motion);
    }
    synth::set_voice(v);
}

void show_patch()
{
    synth_ui::set_patch(synth::voice_name(voice_idx), voice_idx, synth::voice_count(), synth::algorithm(), synth::feedback());
    for (int i = 0; i < 4; i++) synth_ui::set_macro((synth_ui::Macro)i, macro[i]);
}

void choose_voice(int idx)
{
    voice_idx = ((idx % synth::voice_count()) + synth::voice_count()) % synth::voice_count();
    synth::all_notes_off();
    synth::select_voice(voice_idx);
    vTaskDelay(pdMS_TO_TICKS(20));                    // the engine applies the program change in its own task
    synth::get_voice(base_voice);
    for (int i = 0; i < 4; i++) macro[i] = 50;
    settings::set_int("synth_voice", voice_idx);
    show_patch();
}

/** A playable random voice: musical ratios, sane envelopes, one of the 32 algorithms. */
void random_voice()
{
    static const int ratios[] = {1, 1, 1, 2, 2, 3, 4, 5, 7};
    uint8_t v[synth::VOICE_PARAMS] = {};
    const int alg = 1 + esp_random() % 32;
    v[G_ALG] = alg - 1;
    v[135] = esp_random() % 8;                        // feedback
    v[136] = 1;                                       // oscillator sync
    v[G_LFS] = 35; v[138] = 0; v[G_LPMD] = esp_random() % 12; v[G_LAMD] = 0; v[141] = 0; v[142] = 0; v[143] = 3;
    v[144] = 24;                                      // transpose: middle C
    for (int op = 1; op <= 6; op++) {
        uint8_t *o = v + op_off(op);
        const bool car = is_carrier(alg, op);
        o[0] = 60 + esp_random() % 40; o[1] = 30 + esp_random() % 70; o[2] = 20 + esp_random() % 60; o[3] = 40 + esp_random() % 55;
        o[4] = 99; o[5] = 70 + esp_random() % 30; o[6] = car ? 40 + esp_random() % 60 : esp_random() % 99; o[7] = 0;
        o[8] = 39; o[9] = 0; o[10] = 0; o[11] = 0; o[12] = 0;      // no keyboard scaling
        o[13] = esp_random() % 4;                                  // rate scaling
        o[14] = 0; o[15] = 2 + esp_random() % 4;                   // AMS, velocity sensitivity
        o[OP_OL] = car ? 90 + esp_random() % 10 : 35 + esp_random() % 50;
        o[17] = 0; o[18] = ratios[esp_random() % (sizeof ratios / sizeof ratios[0])]; o[19] = 0; o[20] = 7 - (int)(esp_random() % 3) + 1;
    }
    for (int i = 126; i < 130; i++) v[i] = 50;        // pitch envelope flat
    for (int i = 130; i < 134; i++) v[i] = 50;
    snprintf((char *)v + G_NAME, 11, "RANDOM %02d", (int)(esp_random() % 100));
    memcpy(base_voice, v, sizeof base_voice);
    for (int i = 0; i < 4; i++) macro[i] = 50;
    synth::all_notes_off();
    synth::set_voice(v);
    synth_ui::set_patch((const char *)v + G_NAME, voice_idx, synth::voice_count(), alg, v[135]);
    for (int i = 0; i < 4; i++) synth_ui::set_macro((synth_ui::Macro)i, 50);
    ESP_LOGI(TAG, "random voice: algorithm %d", alg);
}

/** Load the first .syx bank found on the SD card, if there is a card and a bank. */
bool load_sd_bank()
{
    static bool mounted = false;
    if (!mounted) {
        static bool no_card = false;                              // one mount attempt per boot
        if (no_card) return false;
        if (bsp_sdcard_mount() != ESP_OK) { ESP_LOGI(TAG, "no SD card"); no_card = true; return false; }
        mounted = true;
    }
    const char *dir = BSP_SD_MOUNT_POINT "/tab5/synth";
    DIR *d = opendir(dir);
    if (!d) { ESP_LOGI(TAG, "no %s on the card", dir); return false; }
    char path[128] = "";
    while (dirent *e = readdir(d)) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".syx")) { snprintf(path, sizeof path, "%s/%s", dir, e->d_name); break; }
    }
    closedir(d);
    if (!path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[6];
    size_t got = fread(hdr, 1, 6, f);
    bool ok = false;
    if (got == 6 && hdr[0] == 0xf0 && hdr[1] == 0x43 && hdr[3] == 0x09) ok = fread(sd_bank, 1, 4096, f) == 4096;   // full bulk dump
    else if (got == 6) { memcpy(sd_bank, hdr, 6); ok = fread(sd_bank + 6, 1, 4090, f) == 4090; }              // raw 4096 bytes
    fclose(f);
    if (ok) { have_sd_bank = true; ESP_LOGI(TAG, "loaded bank %s", path); }
    return ok;
}

// ------------------------------------------------------------------ UI handlers (LVGL task; the engine calls are cheap)
void on_key(synth_ui::Key k)
{
    switch (k) {
    case synth_ui::KEY_PREV:   choose_voice(voice_idx - 1); break;
    case synth_ui::KEY_NEXT:   choose_voice(voice_idx + 1); break;
    case synth_ui::KEY_RANDOM: random_voice(); break;
    case synth_ui::KEY_PANIC:  synth::all_notes_off(); break;
    case synth_ui::KEY_OCT_DOWN: if (octave_base > 24) { octave_base -= 12; synth_ui::set_octave(octave_base); } break;
    case synth_ui::KEY_OCT_UP:   if (octave_base < 84) { octave_base += 12; synth_ui::set_octave(octave_base); } break;
    case synth_ui::KEY_FAV: case synth_ui::KEY_EXPERT: break;   // next milestone
    }
}
void on_note(int note, bool on) { if (on) synth::note_on(note, 100); else synth::note_off(note); }
void on_macro(synth_ui::Macro m, int value, bool) { macro[m] = value; apply_macros(); }

// ------------------------------------------------------------------ lifecycle
TaskHandle_t meter_task_h = nullptr; volatile bool active = false;
void meter_task(void *)
{
    while (active) { synth_ui::set_meter(synth::last_peak(), synth::active_voices()); vTaskDelay(pdMS_TO_TICKS(66)); }
    meter_task_h = nullptr; vTaskDelete(nullptr);
}

void on_enter()
{
    stream::stop();                                   // the radio releases the codec
    if (!synth::start()) { synth_ui::set_status("ENGINE FAILED"); return; }
    if (load_sd_bank()) synth::load_bank(sd_bank, sizeof sd_bank);
    voice_idx = settings::get_int("synth_voice", 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    choose_voice(voice_idx);
    synth_ui::set_octave(octave_base);
    active = true;
    if (!meter_task_h) xTaskCreatePinnedToCore(meter_task, "synth_meter", 4 * 1024, nullptr, 3, &meter_task_h, 0);
}

void on_exit()
{
    active = false;
    for (int i = 0; i < 30 && meter_task_h; i++) vTaskDelay(pdMS_TO_TICKS(10));
    synth::stop();
}

const char *status() { return synth::running() ? "ENGINE ON" : ""; }

} // namespace

const App synth_app = { "synth", "fm synth", "Six-operator synth, sequencer, songs", LV_SYMBOL_LOOP, synth_ui::init, on_enter, on_exit, status };

namespace synth_app_ns {
void register_console()
{
    console::add("note",   [](const char *a, int v) { if (strstr(a, "off")) synth::note_off(v); else synth::note_on(v, 100); }, "note N [off]: play/stop a MIDI note");
    console::add("voice",  [](const char *, int v) { choose_voice(v - 1); }, "voice N: select bank voice");
    console::add("random", [](const char *, int) { random_voice(); }, "random voice");
    console::add("macro",  [](const char *a, int m) { const char *sp = strchr(a, ' '); if (sp) { macro[m & 3] = atoi(sp + 1); apply_macros(); show_patch(); } }, "macro I V: set dial I (0-3) to V (0-100)");
    console::add("panic",  [](const char *, int) { synth::all_notes_off(); }, "all notes off");
    console::add("dump",   [](const char *, int) {                           // hex dump of the live voice
        ESP_LOGI(TAG, "heap integrity: %s", heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT");
        uint8_t v[synth::VOICE_PARAMS]; synth::get_voice(v);
        char line[3 * 21 + 1];
        for (int row = 0; row < 8; row++) {
            int n = 0;
            for (int i = row * 21; i < (row + 1) * 21 && i < synth::VOICE_PARAMS; i++) n += snprintf(line + n, sizeof line - n, "%02x ", v[i]);
            ESP_LOGI(TAG, "voice[%3d..] %s", row * 21, line);
        }
    }, "hex dump of the voice the engine is playing");
    console::add("peak",   [](const char *, int) {
        uint32_t blocks, bytes; int pending;
        synth::debug_stats(blocks, bytes, pending);
        ESP_LOGI(TAG, "peak %d, %d voices live, engine %s, %lu blocks, midi in %lu bytes, %d pending",
                 synth::last_peak(), synth::active_voices(), synth::running() ? "on" : "off",
                 (unsigned long)blocks, (unsigned long)bytes, pending);
    }, "engine output level and render statistics");
}
}
