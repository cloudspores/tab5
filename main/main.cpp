// Tab5 internet radio.
// Display + LVGL, WiFi through the on-board ESP32-C6, HTTP streams to the speaker or a Sonos room.

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#include "secrets.h"
#include "ui.h"
#include "stream.h"
#include "c6_update.h"
#include "settings.h"
#include "stations.h"
#include "bridge.h"

static const char *TAG = "radio";
using stations::Station;

// ---------------------------------------------------------------- state
static std::vector<Station> band;          // what the TUNE module shows
static int band_idx = 0;
static Station current = {};
static std::vector<Station> results;       // last search
static std::vector<std::string> outputs = {"TAB5"};
static int output_idx = 0;
static int volume = 60;
static bool muted = false;
static int brightness = 50;

static void apply_brightness(int pct)
{
    brightness = pct < 10 ? 10 : pct > 100 ? 100 : pct;
    bsp_display_brightness_set(brightness);
    settings::set_int("light", brightness);
    ui::set_brightness_text(brightness);
}

static EventGroupHandle_t wifi_events;
static constexpr int WIFI_CONNECTED = BIT0;

// UI callbacks post commands; the control task does the work (network calls included).
enum CmdType { CMD_KEY, CMD_TUNE, CMD_PRESET, CMD_SEARCH, CMD_RESULT, CMD_OUTPUT, CMD_VOLUME, CMD_LIGHT, CMD_POLL };
struct Cmd { CmdType type; int a; int b; char text[64]; };
static QueueHandle_t cmds;

static void post(CmdType t, int a = 0, int b = 0, const char *text = nullptr)
{
    Cmd c = {t, a, b, ""};
    if (text) strlcpy(c.text, text, sizeof c.text);
    xQueueSend(cmds, &c, 0);
}

// ---------------------------------------------------------------- helpers
static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap @%s: internal %u KB (largest %u KB), dma %u KB, psram %u KB", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

static std::vector<std::string> names_of(const std::vector<Station> &v)
{
    std::vector<std::string> n;
    for (auto &s : v) n.push_back(s.name);
    return n;
}

static void refresh_presets()
{
    std::vector<std::string> n;
    for (int i = 0; i < stations::PRESET_SLOTS; i++) n.push_back(stations::preset(i).name);
    ui::set_presets(n);
}

static bool output_is_sonos() { return output_idx > 0; }
static bool user_paused = false;

// Same signal M5Stack's firmware uses: pulse pin 4 of the second IO expander three times.
static void power_off()
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

static void play(const Station &s)
{
    current = s;
    stations::save_last(s);
    ui::set_station(s.name);
    ui::set_title("");
    user_paused = false;
    ui::set_playing(true);
    if (output_is_sonos()) {
        stream::stop();
        ui::set_status("SONOS");
        ui::set_format(s.codec, s.bitrate, 44100, 2);
        if (!bridge::sonos_play_url(outputs[output_idx].c_str(), s.url, s.name)) ui::set_status("SONOS FAILED");
    } else {
        stream::play({s.name, s.url});
    }
}

static void set_output(int idx)
{
    const int prev = output_idx;
    output_idx = ((idx % (int)outputs.size()) + outputs.size()) % outputs.size();
    if (output_idx == prev) return;
    settings::set_str("out", outputs[output_idx].c_str());
    ui::set_output(outputs[output_idx].c_str());
    if (prev > 0) bridge::sonos_stop(outputs[prev].c_str());     // silence the room we left
    if (output_is_sonos()) bridge::sonos_volume(outputs[output_idx].c_str(), volume);
    if (current.name[0]) play(current);
}

static void tune(int idx)
{
    if (band.empty()) return;
    band_idx = ((idx % (int)band.size()) + band.size()) % band.size();
    ui::set_band_current(band_idx);
    play(band[band_idx]);
}

static void do_search(const char *query, const char *cc)
{
    ui::set_status("SEARCHING");
    if (!stations::search(query, cc, results, 12)) { ui::set_status("SEARCH FAILED"); return; }
    if (query && *query) {
        ui::show_results(names_of(results));   // typed search: pick from the overlay
    } else {
        band = results; band_idx = -1;         // country top list: becomes the band
        ui::set_band(names_of(band), -1);
        ui::close_search();
    }
    ui::set_status(stream::playing() ? "ON AIR" : output_is_sonos() ? "SONOS" : "IDLE");
}

// ---------------------------------------------------------------- UI handlers (LVGL task)
static void on_key(ui::Key k)    { post(CMD_KEY, k); }
static void on_tune(int i)       { post(CMD_TUNE, i); }
static void on_preset(int s, bool store) { post(CMD_PRESET, s, store); }
static void on_search(const char *t)     { post(CMD_SEARCH, 0, 0, t); }
static void on_result(int i)     { post(CMD_RESULT, i); }
static void on_dial(int d, int v) { if (d == ui::DIAL_VOL) post(CMD_VOLUME, v); else post(CMD_TUNE, v); }

// ---------------------------------------------------------------- control task
static void control_task(void *)
{
    Cmd c;
    int poll = 0;
    for (;;) {
        if (xQueueReceive(cmds, &c, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // periodic: clock + Sonos now-playing
            time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
            char buf[8]; strftime(buf, sizeof buf, tm.tm_year > 100 ? "%H:%M" : "--:--", &tm);
            ui::set_clock(buf);
            if (output_is_sonos() && ++poll % 5 == 0) {
                std::string t, a; bool playing = false;
                if (bridge::sonos_now_playing(outputs[output_idx].c_str(), t, a, playing)) {
                    std::string line = a.empty() ? t : a + " - " + t;
                    ui::set_title(line.c_str());
                    ui::set_status(playing ? "ON AIR" : "SONOS");
                }
            }
            continue;
        }
        switch (c.type) {
        case CMD_KEY:
            switch ((ui::Key)c.a) {
            case ui::KEY_VOL_DOWN: volume -= 5; if (volume < 0) volume = 0; goto vol;
            case ui::KEY_VOL_UP:   volume += 5; if (volume > 100) volume = 100; goto vol;
            vol:
                settings::set_int("vol", volume);
                if (output_is_sonos()) bridge::sonos_volume(outputs[output_idx].c_str(), volume);
                else stream::set_volume(volume);
                ui::set_volume(volume, muted);
                break;
            case ui::KEY_MUTE:
                muted = !muted; settings::set_int("mute", muted);
                stream::set_mute(muted); ui::set_volume(volume, muted);
                break;
            case ui::KEY_OUTPUT: set_output(output_idx + 1); break;
            case ui::KEY_LIGHT:  apply_brightness(brightness >= 100 ? 20 : brightness + 20); break;
            case ui::KEY_PLAY:
                if (user_paused) { play(current); }
                else {
                    user_paused = true; ui::set_playing(false);
                    if (output_is_sonos()) { bridge::sonos_stop(outputs[output_idx].c_str()); ui::set_status("PAUSED"); }
                    else { stream::stop(); ui::set_status("PAUSED"); }
                }
                break;
            case ui::KEY_POWER: power_off(); break;
            case ui::KEY_NEXT:   tune(band_idx + 1); break;
            case ui::KEY_SEARCH: ui::open_search(); break;
            case ui::KEY_CR:     do_search("", "CR"); break;
            case ui::KEY_HOME:
                band = stations::builtin(); band_idx = -1;
                for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, current.url)) band_idx = i;
                ui::set_band(names_of(band), band_idx);
                break;
            }
            break;
        case CMD_TUNE: tune(c.a); break;
        case CMD_OUTPUT: set_output(c.a); break;
        case CMD_LIGHT: apply_brightness(c.a); break;
        case CMD_VOLUME:
            volume = c.a < 0 ? 0 : c.a > 100 ? 100 : c.a;
            settings::set_int("vol", volume);
            if (output_is_sonos()) bridge::sonos_volume(outputs[output_idx].c_str(), volume); else stream::set_volume(volume);
            ui::set_volume(volume, muted);
            break;
        case CMD_PRESET:
            if (c.b) { if (current.name[0]) { stations::set_preset(c.a, current); refresh_presets(); } }
            else if (stations::preset(c.a).name[0]) {
                Station s = stations::preset(c.a);
                band_idx = -1;
                for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, s.url)) band_idx = i;
                ui::set_band_current(band_idx);
                play(s);
            }
            break;
        case CMD_SEARCH: do_search(c.text, ""); break;
        case CMD_RESULT:
            if (c.a >= 0 && c.a < (int)results.size()) {
                band = results; band_idx = c.a;
                ui::set_band(names_of(band), band_idx);
                ui::close_search();
                play(band[band_idx]);
            }
            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- serial console (USB)
// Lines on the USB serial console drive the same commands as the touch UI:
//   next | tune N | preset N | store N | out [N] | vol N | mute | search TEXT | cr | home | result N | status
static void console_task(void *)
{
    char line[96]; int n = 0;
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (ch == '\r') continue;
        if (ch != '\n') { if (n < (int)sizeof line - 1) line[n++] = ch; continue; }
        line[n] = 0; n = 0;
        char *arg = strchr(line, ' '); if (arg) *arg++ = 0;
        int v = arg ? atoi(arg) : 0;
        if (!strcmp(line, "next")) post(CMD_KEY, ui::KEY_NEXT);
        else if (!strcmp(line, "tune")) post(CMD_TUNE, v - 1);
        else if (!strcmp(line, "preset")) post(CMD_PRESET, v - 1, 0);
        else if (!strcmp(line, "store")) post(CMD_PRESET, v - 1, 1);
        else if (!strcmp(line, "out")) { if (arg) post(CMD_OUTPUT, v - 1); else post(CMD_KEY, ui::KEY_OUTPUT); }
        else if (!strcmp(line, "vol")) post(CMD_VOLUME, v);
        else if (!strcmp(line, "bright")) post(CMD_LIGHT, v);
        else if (!strcmp(line, "play")) post(CMD_KEY, ui::KEY_PLAY);
        else if (!strcmp(line, "off")) post(CMD_KEY, ui::KEY_POWER);
        else if (!strcmp(line, "mute")) post(CMD_KEY, ui::KEY_MUTE);
        else if (!strcmp(line, "search")) post(CMD_SEARCH, 0, 0, arg ? arg : "");
        else if (!strcmp(line, "cr")) post(CMD_KEY, ui::KEY_CR);
        else if (!strcmp(line, "home")) post(CMD_KEY, ui::KEY_HOME);
        else if (!strcmp(line, "result")) post(CMD_RESULT, v - 1);
        else if (!strcmp(line, "status")) {
            ESP_LOGI(TAG, "station=%s url=%s output=%s vol=%d mute=%d band=%d/%d results=%d",
                     current.name, current.url, outputs[output_idx].c_str(), volume, muted, band_idx + 1, (int)band.size(), (int)results.size());
            for (size_t i = 0; i < outputs.size(); i++) ESP_LOGI(TAG, "  out %d: %s", (int)i + 1, outputs[i].c_str());
            for (size_t i = 0; i < results.size(); i++) ESP_LOGI(TAG, "  result %d: %s [%s %dk %s]", (int)i + 1, results[i].name, results[i].codec, results[i].bitrate, results[i].country);
            log_heap("status");
        }
        else if (line[0]) ESP_LOGW(TAG, "unknown command: %s", line);
    }
}

// ---------------------------------------------------------------- WiFi
static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
        ui::set_status("WIFI RETRY");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED);
    }
}

static void wifi_start()
{
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    vTaskDelay(pdMS_TO_TICKS(200));
    ui::set_status("C6 LINK");
    if (esp_hosted_init() != 0) ESP_LOGE(TAG, "esp_hosted_init failed");
    if (esp_hosted_connect_to_slave() != 0) ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed");
    c6::update_if_needed();

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ui::set_status("WIFI JOIN");
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------------------------------------------------------------- boot
extern "C" void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings::init();
    volume = settings::get_int("vol", 60);
    muted  = settings::get_int("mute", 0) != 0;
    setenv("TZ", "CST6", 1); tzset();     // Costa Rica: UTC-6, no daylight saving

    // A USB reset does not power-cycle the panel; cycle the rails so the BSP's probe succeeds.
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_feature_enable(BSP_FEATURE_TOUCH, false);
    bsp_feature_enable(BSP_FEATURE_LCD, false);
    vTaskDelay(pdMS_TO_TICKS(300));
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    vTaskDelay(pdMS_TO_TICKS(300));

    // Display: portrait-native panel rotated to landscape by the PPA; full-frame buffers in PSRAM.
    bsp_display_cfg_t dcfg = {};
    dcfg.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    dcfg.lvgl_port_cfg.task_affinity = 1;
    dcfg.lvgl_port_cfg.task_stack = 12 * 1024;
    dcfg.buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
    dcfg.double_buffer = true;
    dcfg.flags.buff_dma = true;
    dcfg.flags.buff_spiram = true;
    dcfg.flags.sw_rotate = true;
    lv_display_t *disp = bsp_display_start_with_config(&dcfg);
    bsp_display_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    bsp_display_unlock();
    bsp_display_backlight_on();
    brightness = settings::get_int("light", 50);
    bsp_display_brightness_set(brightness);
    ui::init();
    ui::on_key(on_key); ui::on_tune(on_tune); ui::on_preset(on_preset); ui::on_search(on_search); ui::on_result(on_result); ui::on_dial(on_dial);
    ui::set_volume(volume, muted);
    ui::set_brightness_text(brightness);

    stations::load_presets();
    refresh_presets();
    band = stations::builtin();
    if (!stations::load_last(current)) current = band[0];
    band_idx = -1;
    for (size_t i = 0; i < band.size(); i++) if (!strcmp(band[i].url, current.url)) band_idx = i;
    ui::set_band(names_of(band), band_idx);
    ui::set_station(current.name);
    log_heap("ui");

    stream::init();
    stream::set_volume(volume);
    stream::set_mute(muted);

    cmds = xQueueCreate(16, sizeof(Cmd));
    xTaskCreatePinnedToCore(control_task, "control", 12 * 1024, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4 * 1024, nullptr, 3, nullptr, 0);

    wifi_start();
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
    log_heap("wifi");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Outputs: TAB5 plus whatever Sonos rooms the bridge reports (bridge optional).
    std::vector<std::string> rooms;
    if (bridge::sonos_rooms(rooms)) for (auto &r : rooms) outputs.push_back(r);
    char saved[64] = "";
    settings::get_str("out", saved, sizeof saved);
    for (size_t i = 0; i < outputs.size(); i++) if (outputs[i] == saved) output_idx = i;
    ui::set_output(outputs[output_idx].c_str());
    ESP_LOGI(TAG, "%d outputs, using %s", (int)outputs.size(), outputs[output_idx].c_str());

    play(current);
}
