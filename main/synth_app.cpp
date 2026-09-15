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
 *
 * The app has two pages, SOUND and PLAY, that share the engine and the performance layer
 * (scale lock, chord pads, arpeggiator in synth_perf). Every key press from either page goes
 * through the performance layer, so the arpeggiator and scale lock apply everywhere.
 */
#include "synth_app.h"
#include "synth_engine.h"
#include "synth_ui.h"
#include "synth_play_ui.h"
#include "synth_perf.h"
#include "synth_keys.h"
#include "synth_relay.h"
#include "topbar.h"
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
    synth_play_ui::set_patch(synth::voice_name(voice_idx), voice_idx, synth::voice_count());
    for (int i = 0; i < 4; i++) synth_ui::set_macro((synth_ui::Macro)i, macro[i]);
}

void choose_voice(int idx)
{
    voice_idx = ((idx % synth::voice_count()) + synth::voice_count()) % synth::voice_count();
    perf::all_off();
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

// ------------------------------------------------------------------ pages
enum Page { PAGE_SOUND = 0, PAGE_PLAY = 1 };
int page = PAGE_SOUND;

/** Show one of the synth's pages (both exist once the app screen was requested). */
void show_page(int p)
{
    page = p;
    theme::lock();
    lv_screen_load(p == PAGE_PLAY ? synth_play_ui::init() : synth_ui::init());
    theme::unlock();
    settings::set_int("synth_page", page);
}

/** App entry point for the launcher: builds both pages and returns the last-used one. */
lv_obj_t *app_screen()
{
    perf::init();
    lv_obj_t *sound = synth_ui::init();
    lv_obj_t *play = synth_play_ui::init();
    page = settings::get_int("synth_page", PAGE_SOUND);
    return page == PAGE_PLAY ? play : sound;
}

void set_octave(int base)
{
    octave_base = base;
    synth_ui::set_octave(base);
    synth_play_ui::set_octave(base);
}

/** Sounding notes changed (keys, pads or an arpeggiator step): light the keys on both pages. */
void on_perf_change()
{
    uint32_t mask = perf::sounding_mask(octave_base, synth_keys::SPAN);
    synth_ui::highlight(mask);
    synth_play_ui::highlight(mask, perf::active_pad());
}

// ------------------------------------------------------------------ UI handlers (LVGL task; the engine calls are cheap)
void on_key(synth_ui::Key k)
{
    switch (k) {
    case synth_ui::KEY_PREV:   choose_voice(voice_idx - 1); break;
    case synth_ui::KEY_NEXT:   choose_voice(voice_idx + 1); break;
    case synth_ui::KEY_RANDOM: random_voice(); break;
    case synth_ui::KEY_PANIC:  perf::all_off(); break;
    case synth_ui::KEY_PLAY:   show_page(PAGE_PLAY); break;
    case synth_ui::KEY_OCT_DOWN: if (octave_base > 24) set_octave(octave_base - 12); break;
    case synth_ui::KEY_OCT_UP:   if (octave_base < 84) set_octave(octave_base + 12); break;
    case synth_ui::KEY_FAV: case synth_ui::KEY_EXPERT: break;   // later milestones
    }
}
void on_note(int note, bool on) { if (on) perf::key_down(note); else perf::key_up(note); }
void on_output(int index)
{
    const auto &outs = synth_relay::outputs();
    if (index >= 0 && index < (int)outs.size()) synth_relay::select(outs[index].c_str());
}
/** The VOL dial: the Tab5's own level (shared with the radio) or, while relaying, the room's volume. */
void on_volume(int v, bool released)
{
    stream::set_volume(v);
    if (released) { settings::set_int("vol", v); synth_relay::set_volume(v); }
}

/** Mirror the relay's output list and selection into the radio buttons. */
void show_outputs()
{
    const auto &outs = synth_relay::outputs();
    const char *names[synth_ui::MAX_OUTPUTS]; int n = 0, sel = 0;
    for (size_t i = 0; i < outs.size() && n < synth_ui::MAX_OUTPUTS; i++) { if (outs[i] == synth_relay::current()) sel = n; names[n++] = outs[i].c_str(); }
    synth_ui::set_outputs(names, n, sel);
    topbar::set_output(synth_relay::current());
}
void on_macro(synth_ui::Macro m, int value, bool) { macro[m] = value; apply_macros(); }

void on_play_key(synth_play_ui::Key k)
{
    using namespace synth_play_ui;
    switch (k) {
    case KEY_LATCH:    perf::set_latch(!perf::latch()); break;
    case KEY_VOICING:  perf::set_voicing(perf::voicing() % 3 + 1); break;
    case KEY_SPREAD:   perf::set_spread(!perf::spread()); break;
    case KEY_SEVENTHS: perf::set_sevenths(!perf::sevenths()); break;
    case KEY_ARP_OFF: case KEY_ARP_UP: case KEY_ARP_DOWN: case KEY_ARP_RANDOM: perf::set_arp((perf::Arp)(k - KEY_ARP_OFF)); break;
    case KEY_ROOT_DOWN: perf::set_root(perf::root() - 1); break;
    case KEY_ROOT_UP:   perf::set_root(perf::root() + 1); break;
    case KEY_MODE:     perf::set_mode((perf::Mode)((perf::mode() + 1) % perf::MODE_COUNT)); break;
    case KEY_LOCK:     perf::set_scale_lock(!perf::scale_lock()); break;
    case KEY_PREV:     choose_voice(voice_idx - 1); return;
    case KEY_NEXT:     choose_voice(voice_idx + 1); return;
    case KEY_SOUND:    show_page(PAGE_SOUND); return;
    case KEY_OCT_DOWN: if (octave_base > 24) set_octave(octave_base - 12); return;
    case KEY_OCT_UP:   if (octave_base < 84) set_octave(octave_base + 12); return;
    }
    perf::save();
    synth_play_ui::refresh();
}
void on_pad(int degree, bool down) { if (down) perf::pad_down(degree); else perf::pad_up(degree); }
void on_play_dial(synth_play_ui::DialId d, int v, bool released)
{
    switch (d) {
    case synth_play_ui::DIAL_RATE:  perf::set_rate(v); break;
    case synth_play_ui::DIAL_GATE:  perf::set_gate(v); break;
    case synth_play_ui::DIAL_TEMPO: perf::set_tempo(v); break;
    }
    if (released) { perf::save(); synth_play_ui::refresh(); }
}

// ------------------------------------------------------------------ lifecycle
TaskHandle_t meter_task_h = nullptr; volatile bool active = false;
void meter_task(void *)
{
    int shown = -1;
    while (active) {
        synth_ui::set_meter(synth::last_peak(), synth::active_voices(), synth_relay::status());
        if (shown != synth_relay::changed_count()) { shown = synth_relay::changed_count(); show_outputs(); }
        vTaskDelay(pdMS_TO_TICKS(66));
    }
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
    set_octave(octave_base);
    synth_play_ui::refresh();
    synth_relay::start();                             // restores a Sonos output if one was chosen
    synth_relay::discover();
    synth_ui::set_volume(stream::volume_percent());
    active = true;
    if (!meter_task_h) xTaskCreatePinnedToCore(meter_task, "synth_meter", 4 * 1024, nullptr, 3, &meter_task_h, 0);
}

void on_exit()
{
    perf::all_off();
    active = false;
    for (int i = 0; i < 30 && meter_task_h; i++) vTaskDelay(pdMS_TO_TICKS(10));
    synth_relay::stop();                              // hands the Sonos room back
    synth::stop();
}

const char *status() { return synth::running() ? "ENGINE ON" : ""; }

} // namespace

const App synth_app = { "synth", "fm synth", "Six-operator synth, sequencer, songs", LV_SYMBOL_LOOP, app_screen, on_enter, on_exit, status };

namespace synth_app_ns {
void register_console()
{
    synth_ui::on_key(on_key); synth_ui::on_note(on_note); synth_ui::on_macro(on_macro);
    synth_ui::on_output(on_output); synth_ui::on_volume(on_volume);
    synth_play_ui::on_key(on_play_key); synth_play_ui::on_pad(on_pad); synth_play_ui::on_note(on_note); synth_play_ui::on_dial(on_play_dial);
    perf::on_change(on_perf_change);
    console::add("note",   [](const char *a, int v) { if (strstr(a, "off")) perf::key_up(v); else perf::key_down(v); }, "note N [off]: press/release a key (scale lock and arp apply)");
    console::add("voice",  [](const char *, int v) { choose_voice(v - 1); }, "voice N: select bank voice");
    console::add("random", [](const char *, int) { random_voice(); }, "random voice");
    console::add("macro",  [](const char *a, int m) { const char *sp = strchr(a, ' '); if (sp) { macro[m & 3] = atoi(sp + 1); apply_macros(); show_patch(); } }, "macro I V: set dial I (0-3) to V (0-100)");
    console::add("panic",  [](const char *, int) { perf::all_off(); }, "all notes off");
    console::add("relay",  [](const char *a, int) {
        if (!*a || !strcmp(a, "off")) synth_relay::select(synth_relay::LOCAL); else synth_relay::select(a);
        ESP_LOGI(TAG, "output -> %s", synth_relay::current()); }, "relay ROOM|off: send the synth to a Sonos room");
    console::add("svol",   [](const char *, int v) { on_volume(v, true); synth_ui::set_volume(v); }, "svol 0-100: synth volume (local or room)");
    console::add("outputs", [](const char *, int) { for (auto &o : synth_relay::outputs()) ESP_LOGI(TAG, "  %s%s", o.c_str(), o == synth_relay::current() ? "  <- current" : ""); }, "list synth outputs");
    console::add("page",   [](const char *a, int) { show_page(strcmp(a, "play") == 0 ? PAGE_PLAY : PAGE_SOUND); }, "page sound|play: switch the synth page");
    console::add("pad",    [](const char *a, int v) { if (strstr(a, "off")) perf::pad_up(v - 1); else perf::pad_down(v - 1); }, "pad N [off]: press/release chord pad 1-7");
    console::add("arp",    [](const char *a, int) {
        perf::Arp m = !strcmp(a, "up") ? perf::ARP_UP : !strcmp(a, "down") ? perf::ARP_DOWN : !strcmp(a, "rnd") ? perf::ARP_RANDOM : perf::ARP_OFF;
        perf::set_arp(m); perf::save(); synth_play_ui::refresh(); }, "arp off|up|down|rnd");
    console::add("tempo",  [](const char *, int v) { perf::set_tempo(v); perf::save(); synth_play_ui::refresh(); }, "tempo BPM");
    console::add("rate",   [](const char *, int v) { perf::set_rate(v); perf::save(); synth_play_ui::refresh(); }, "rate 0-6: 1/2 1/4 1/8 1/8T 1/16 1/16T 1/32");
    console::add("gate",   [](const char *, int v) { perf::set_gate(v); perf::save(); synth_play_ui::refresh(); }, "gate 10-100 percent");
    console::add("scale",  [](const char *a, int v) {
        const char *sp = strchr(a, ' ');
        perf::set_root(v); if (sp) perf::set_mode((perf::Mode)atoi(sp + 1));
        perf::save(); synth_play_ui::refresh();
        ESP_LOGI(TAG, "scale %s", perf::scale_name()); }, "scale ROOT [MODE]: root 0-11 (C=0), mode 0-6");
    console::add("latch",  [](const char *a, int) { perf::set_latch(!strcmp(a, "on")); perf::save(); synth_play_ui::refresh(); }, "latch on|off");
    console::add("lock",   [](const char *a, int) { perf::set_scale_lock(!strcmp(a, "on")); perf::save(); synth_play_ui::refresh(); }, "lock on|off: keyboard scale lock");
    console::add("chords", [](const char *, int) {
        for (int i = 0; i < perf::PADS; i++) { char n[8], sy[12]; perf::chord_label(i, n, sy); ESP_LOGI(TAG, "pad %d: %-5s %s", i + 1, n, sy); } }, "list the chord pads for the current scale");
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
