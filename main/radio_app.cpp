/**
 * @file radio_app.cpp
 * @brief Internet radio app logic.
 *
 * Structure:
 *  - UI callbacks (LVGL task) only post commands to a queue.
 *  - A control task owns all state and does the work, including network calls to the bridge and
 *    the Radio Browser directory, so nothing slow ever runs inside LVGL.
 *  - Playback itself lives in stream.cpp; Sonos handoff goes through bridge.cpp.
 *
 * Persisted in NVS: last station, volume, mute, brightness, output, presets.
 */
#include "radio_app.h"
#include "radio_ui.h"
#include "stream.h"
#include "stations.h"
#include "settings.h"
#include "bridge.h"
#include "console.h"
#include "topbar.h"
#include "launcher.h"
#include "theme.h"
#include "net.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "radio";
using stations::Station;

namespace {

// ------------------------------------------------------------------ state (control task only)

std::vector<Station> band;                 ///< what the TUNE module shows
int band_idx = 0;                          ///< index into band, -1 when the current station isn't on it
Station current = {};                      ///< the station being played (or handed to Sonos)
std::vector<Station> results;              ///< last directory search, shown in the overlay
std::vector<std::string> outputs = {"TAB5"};   ///< "TAB5" followed by Sonos room names from the bridge
int output_idx = 0;
int volume = 60;                           ///< 0..100, applies to the speaker or the selected room
bool muted = false;
bool user_paused = false;                  ///< play/pause key state; distinct from a dropped stream
int brightness = 50;

// ------------------------------------------------------------------ command queue

enum CmdType { CMD_KEY, CMD_TUNE, CMD_PRESET, CMD_SEARCH, CMD_RESULT, CMD_OUTPUT, CMD_VOLUME, CMD_LIGHT, CMD_DISCOVER, CMD_RESUME };
struct Cmd { CmdType type; int a; int b; char text[64]; };
QueueHandle_t cmds;

void post(CmdType t, int a = 0, int b = 0, const char *text = nullptr)
{
    Cmd c = {t, a, b, ""};
    if (text) strlcpy(c.text, text, sizeof c.text);
    xQueueSend(cmds, &c, 0);
}

// ------------------------------------------------------------------ helpers

bool output_is_sonos() { return output_idx > 0; }

std::vector<std::string> names_of(const std::vector<Station> &v)
{
    std::vector<std::string> n;
    for (auto &s : v) n.push_back(s.name);
    return n;
}

void refresh_presets()
{
    std::vector<std::string> n;
    for (int i = 0; i < stations::PRESET_SLOTS; i++) n.push_back(stations::preset(i).name);
    ui::set_presets(n);
}

void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap @%s: internal %u KB (largest %u KB), dma %u KB, psram %u KB", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

void apply_brightness(int pct)
{
    brightness = pct < 10 ? 10 : pct > 100 ? 100 : pct;
    bsp_display_brightness_set(brightness);
    settings::set_int("light", brightness);
}

/** Play a station on the selected output: local decode, or hand the URL to the Sonos room. */
void play(const Station &s)
{
    current = s;
    stations::save_last(s);
    if (!net::connected()) { ui::set_station(s.name); ui::set_status("NO WIFI"); return; }   // radio::start() plays once online
    user_paused = false;
    ui::set_station(s.name);
    ui::set_title("");
    ui::set_playing(true);
    if (output_is_sonos()) {
        stream::stop();
        ui::set_status("SONOS");
        ui::set_format(s.codec, s.bitrate, 44100, 2);
        if (!bridge::sonos_play_url(outputs[output_idx].c_str(), s.url, s.name)) ui::set_status("SONOS FAILED");
    } else {
        stream::play({s.name, s.url});
    }
    launcher::refresh();
}

/** Switch output. Silences the room we leave, applies the volume to the room we join. */
void set_output(int idx)
{
    const int prev = output_idx;
    output_idx = ((idx % (int)outputs.size()) + outputs.size()) % outputs.size();
    if (output_idx == prev) return;
    settings::set_str("out", outputs[output_idx].c_str());
    ui::set_output(outputs[output_idx].c_str());
    if (prev > 0) bridge::sonos_stop(outputs[prev].c_str());
    if (output_is_sonos()) bridge::sonos_volume(outputs[output_idx].c_str(), volume);
    if (current.name[0]) play(current);
}

void tune(int idx)
{
    if (band.empty()) return;
    band_idx = ((idx % (int)band.size()) + band.size()) % band.size();
    ui::set_band_current(band_idx);
    play(band[band_idx]);
}

/** Directory search. A typed query fills the overlay; an empty query with a country replaces the band. */
void do_search(const char *query, const char *cc)
{
    ui::set_status("SEARCHING");
    if (!stations::search(query, cc, results, 12)) { ui::set_status("SEARCH FAILED"); return; }
    if (query && *query) {
        ui::show_results(names_of(results));
    } else {
        band = results; band_idx = -1;
        ui::set_band(names_of(band), -1);
        ui::close_search();
    }
    ui::set_status(stream::playing() ? "ON AIR" : output_is_sonos() ? "SONOS" : "IDLE");
}

void set_volume(int v)
{
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    settings::set_int("vol", volume);
    if (output_is_sonos()) bridge::sonos_volume(outputs[output_idx].c_str(), volume);
    else stream::set_volume(volume);
    ui::set_volume(volume, muted);
}

void toggle_play()
{
    if (user_paused) { play(current); return; }
    user_paused = true;
    ui::set_playing(false);
    if (output_is_sonos()) bridge::sonos_stop(outputs[output_idx].c_str()); else stream::stop();
    ui::set_status("PAUSED");
    launcher::refresh();
}

/** Same signal M5Stack's firmware uses: pulse pin 4 of the second IO expander three times. */
void power_off()
{
    ESP_LOGW(TAG, "power off");
    ui::set_status("POWER OFF");
    stream::stop();
    if (output_is_sonos()) bridge::sonos_stop(outputs[output_idx].c_str());
    bsp_display_brightness_set(0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_handle_t ex = bsp_io_expander1_init();
    esp_io_expander_set_dir(ex, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_OUTPUT);
    for (int i = 0; i < 3; i++) {
        esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_4, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_4, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/** After WiFi: ask the bridge for Sonos rooms, restore the saved output, start playing. */
void discover_outputs()
{
    std::vector<std::string> rooms;
    if (bridge::sonos_rooms(rooms)) for (auto &r : rooms) outputs.push_back(r);
    char saved[64] = "";
    settings::get_str("out", saved, sizeof saved);
    for (size_t i = 0; i < outputs.size(); i++) if (outputs[i] == saved) output_idx = i;
    ui::set_output(outputs[output_idx].c_str());
    ESP_LOGI(TAG, "%d outputs, using %s", (int)outputs.size(), outputs[output_idx].c_str());
    // Only start playing if the radio is the app on screen: another audio app may own the codec.
    if (!strcmp(launcher::current(), "radio")) play(current);   // otherwise on_enter resumes later
}

/** Entering the radio screen: pick up where we left off unless the user paused deliberately. */
void resume_if_idle()
{
    if (current.name[0] && net::connected() && !stream::playing() && !output_is_sonos() && !user_paused) play(current);
}

// ------------------------------------------------------------------ UI callbacks (LVGL task)

void on_key(ui::Key k)              { post(CMD_KEY, k); }
void on_tune(int i)                 { post(CMD_TUNE, i); }
void on_preset(int s, bool store)   { post(CMD_PRESET, s, store); }
void on_search(const char *t)       { post(CMD_SEARCH, 0, 0, t); }
void on_result(int i)               { post(CMD_RESULT, i); }
void on_dial(int d, int v)          { if (d == ui::DIAL_VOL) post(CMD_VOLUME, v); else post(CMD_TUNE, v); }

// ------------------------------------------------------------------ control task

void handle_key(ui::Key k)
{
    switch (k) {
    case ui::KEY_VOL_DOWN: set_volume(volume - 5); break;
    case ui::KEY_VOL_UP:   set_volume(volume + 5); break;
    case ui::KEY_MUTE:
        muted = !muted; settings::set_int("mute", muted);
        stream::set_mute(muted); ui::set_volume(volume, muted);
        break;
    case ui::KEY_OUTPUT: set_output(output_idx + 1); break;
    case ui::KEY_LIGHT:  apply_brightness(brightness >= 100 ? 20 : brightness + 20); break;
    case ui::KEY_PLAY:   toggle_play(); break;
    case ui::KEY_POWER:  power_off(); break;
    case ui::KEY_NEXT:   tune(band_idx + 1); break;
    case ui::KEY_SEARCH: ui::open_search(); break;
    case ui::KEY_CR:     do_search("", "CR"); break;
    case ui::KEY_HOME:
        band = stations::builtin(); band_idx = -1;
        for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, current.url)) band_idx = i;
        ui::set_band(names_of(band), band_idx);
        break;
    }
}

void handle_preset(int slot, bool store)
{
    if (store) { if (current.name[0]) { stations::set_preset(slot, current); refresh_presets(); } return; }
    if (!stations::preset(slot).name[0]) return;
    Station s = stations::preset(slot);
    band_idx = -1;
    for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, s.url)) band_idx = i;
    ui::set_band_current(band_idx);
    play(s);
}

/** Once a second when idle: clock, and the Sonos room's now-playing text every five seconds. */
void periodic()
{
    static int tick = 0;
    time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
    char buf[8]; strftime(buf, sizeof buf, tm.tm_year > 100 ? "%H:%M" : "--:--", &tm);
    ui::set_clock(buf);
    if (output_is_sonos() && !user_paused && ++tick % 5 == 0) {
        std::string t, a; bool playing = false;
        if (bridge::sonos_now_playing(outputs[output_idx].c_str(), t, a, playing)) {
            std::string line = a.empty() ? t : a + " - " + t;
            ui::set_title(line.c_str());
            ui::set_status(playing ? "ON AIR" : "SONOS");
        }
    }
}

void control_task(void *)
{
    Cmd c;
    for (;;) {
        if (xQueueReceive(cmds, &c, pdMS_TO_TICKS(1000)) != pdTRUE) { periodic(); continue; }
        switch (c.type) {
        case CMD_KEY:      handle_key((ui::Key)c.a); break;
        case CMD_TUNE:     tune(c.a); break;
        case CMD_PRESET:   handle_preset(c.a, c.b != 0); break;
        case CMD_SEARCH:   do_search(c.text, ""); break;
        case CMD_RESULT:
            if (c.a >= 0 && c.a < (int)results.size()) {
                band = results; band_idx = c.a;
                ui::set_band(names_of(band), band_idx);
                ui::close_search();
                play(band[band_idx]);
            }
            break;
        case CMD_OUTPUT:   set_output(c.a); break;
        case CMD_VOLUME:   set_volume(c.a); break;
        case CMD_LIGHT:    apply_brightness(c.a); break;
        case CMD_DISCOVER: discover_outputs(); break;
        case CMD_RESUME:   resume_if_idle(); break;
        }
    }
}

// ------------------------------------------------------------------ console commands

void cmd_status(const char *, int)
{
    ESP_LOGI(TAG, "station=%s url=%s output=%s vol=%d mute=%d band=%d/%d results=%d",
             current.name, current.url, outputs[output_idx].c_str(), volume, muted, band_idx + 1, (int)band.size(), (int)results.size());
    for (size_t i = 0; i < outputs.size(); i++) ESP_LOGI(TAG, "  out %d: %s", (int)i + 1, outputs[i].c_str());
    for (size_t i = 0; i < results.size(); i++)
        ESP_LOGI(TAG, "  result %d: %s [%s %dk %s]", (int)i + 1, results[i].name, results[i].codec, results[i].bitrate, results[i].country);
    log_heap("status");
}

void register_console()
{
    console::add("status", cmd_status, "radio state, outputs, last results, heap");
    console::add("next",   [](const char *, int) { post(CMD_KEY, ui::KEY_NEXT); }, "next station on the band");
    console::add("tune",   [](const char *, int v) { post(CMD_TUNE, v - 1); }, "tune N (band position)");
    console::add("preset", [](const char *, int v) { post(CMD_PRESET, v - 1, 0); }, "preset N");
    console::add("store",  [](const char *, int v) { post(CMD_PRESET, v - 1, 1); }, "store N (current station into preset)");
    console::add("out",    [](const char *a, int v) { if (*a) post(CMD_OUTPUT, v - 1); else post(CMD_KEY, ui::KEY_OUTPUT); }, "out [N] (next output, or output N)");
    console::add("vol",    [](const char *, int v) { post(CMD_VOLUME, v); }, "vol N (0-100)");
    console::add("mute",   [](const char *, int) { post(CMD_KEY, ui::KEY_MUTE); }, "toggle mute");
    console::add("play",   [](const char *, int) { post(CMD_KEY, ui::KEY_PLAY); }, "toggle play/pause");
    console::add("bright", [](const char *, int v) { post(CMD_LIGHT, v); }, "bright N (10-100)");
    console::add("search", [](const char *a, int) { post(CMD_SEARCH, 0, 0, a); }, "search TEXT (Radio Browser)");
    console::add("cr",     [](const char *, int) { post(CMD_KEY, ui::KEY_CR); }, "Costa Rica top stations onto the band");
    console::add("home",   [](const char *, int) { post(CMD_KEY, ui::KEY_HOME); }, "built-in stations onto the band");
    console::add("result", [](const char *, int v) { post(CMD_RESULT, v - 1); }, "result N (play a search result)");
    console::add("off",    [](const char *, int) { post(CMD_KEY, ui::KEY_POWER); }, "power off");
}

const char *app_status() { return stream::playing() ? "PLAYING" : (output_is_sonos() && current.name[0] && !user_paused ? "ON SONOS" : ""); }

} // namespace

namespace {
void on_enter() { post(CMD_RESUME); }
}

const App radio_app = { "radio", "radio", "Internet radio, presets, Sonos output", LV_SYMBOL_AUDIO, ui::init, on_enter, nullptr, app_status };

namespace radio {

void init()
{
    volume = settings::get_int("vol", 60);
    muted  = settings::get_int("mute", 0) != 0;
    brightness = settings::get_int("light", 50);

    theme::lock(); ui::init(); theme::unlock();
    ui::on_key(on_key); ui::on_tune(on_tune); ui::on_preset(on_preset);
    ui::on_search(on_search); ui::on_result(on_result); ui::on_dial(on_dial);
    ui::set_volume(volume, muted);

    stations::load_presets();
    refresh_presets();
    band = stations::builtin();
    if (!stations::load_last(current)) current = band[0];
    band_idx = -1;
    for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, current.url)) band_idx = i;
    ui::set_band(names_of(band), band_idx);
    ui::set_station(current.name);

    stream::init();
    stream::set_volume(volume);
    stream::set_mute(muted);

    cmds = xQueueCreate(16, sizeof(Cmd));
    xTaskCreatePinnedToCore(control_task, "radio", 12 * 1024, nullptr, 5, nullptr, 0);
    register_console();
    log_heap("radio init");
}

void start() { post(CMD_DISCOVER); }

}
