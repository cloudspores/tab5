/**
 * @file translate_app.cpp
 * @brief Live translation app.
 *
 * The device stays simple: it streams 16 kHz microphone audio to the bridge's WebSocket, renders
 * the events that come back (partial transcript, phrase, translation), and plays the returned
 * speech. Segmentation, transcription, translation and synthesis all happen on the Spark.
 *
 * Screen: two panels. THEM (top) holds everything in Spanish, YOU (bottom) everything in English,
 * so each side reads their own language. FLIP turns the top panel 180° for the person opposite.
 * The microphone is muted while a translation plays so the device does not translate itself.
 */
#include "translate_app.h"
#include "theme.h"
#include "topbar.h"
#include "console.h"
#include "bridge.h"
#include "stream.h"
#include "settings.h"
#include "net.h"

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"

static const char *TAG = "translate";

namespace {

// ------------------------------------------------------------------ state
lv_obj_t *scr = nullptr, *panel_them, *panel_you;
lv_obj_t *them_latest, *them_prev, *you_latest, *you_prev, *lbl_hear, *lbl_state, *key_listen, *key_flip, *key_speak;
bool listening = false, flipped = false, speak = true, active = false;
esp_websocket_client_handle_t ws = nullptr;
esp_codec_dev_handle_t mic = nullptr;
bool spk_open = false;
TaskHandle_t mic_task_h = nullptr;
int64_t mute_until_us = 0;                 ///< microphone muted while our own audio plays
uint8_t *audio_buf = nullptr; int audio_len = 0, audio_expected = 0;   ///< incoming translation audio
struct Clip { uint8_t *pcm; int len; };
QueueHandle_t clips = nullptr;            ///< completed translation clips, played by their own task
TaskHandle_t play_task_h = nullptr;

constexpr int RATE = 16000;
constexpr int FRAME_MS = 100;

// ------------------------------------------------------------------ UI helpers
void set_pair(lv_obj_t *latest, lv_obj_t *prev, const char *text)
{
    theme::lock();
    lv_label_set_text(prev, lv_label_get_text(latest));
    lv_label_set_text(latest, text);
    theme::unlock();
}
void set_state(const char *t) { theme::lock(); lv_label_set_text(lbl_state, t); theme::unlock(); }
void set_hearing(const char *t) { theme::lock(); lv_label_set_text(lbl_hear, t); theme::unlock(); }

void style_key(lv_obj_t *k, bool on)
{
    lv_obj_set_style_bg_color(k, lv_color_hex(on ? theme::ORANGE : theme::PANEL), 0);
    lv_obj_set_style_border_color(k, lv_color_hex(on ? theme::ORANGE : theme::LIGHT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(k, 0), lv_color_hex(on ? 0xffffff : theme::INK), 0);
}

// ------------------------------------------------------------------ audio out
void play_pcm16k(const uint8_t *pcm, int len)
{
    auto spk = (esp_codec_dev_handle_t)stream::speaker();
    if (!spk) return;
    if (!spk_open) {
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16; fs.channel = 2;
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        fs.sample_rate = RATE;
        if (esp_codec_dev_open(spk, &fs) != 0) { ESP_LOGE(TAG, "speaker open failed"); return; }
        esp_codec_dev_set_out_vol(spk, stream::volume_percent());
        spk_open = true;
    }
    // Keep the microphone quiet for the duration plus a tail so the room echo is not transcribed.
    mute_until_us = esp_timer_get_time() + (int64_t)len / 2 * 1000000 / RATE + 500000;
    static int16_t st[4096];
    const int16_t *s = (const int16_t *)pcm;
    int n = len / 2, done = 0;
    while (done < n) {
        int take = n - done; if (take > 2048) take = 2048;
        for (int i = 0; i < take; i++) { st[2 * i] = s[done + i]; st[2 * i + 1] = s[done + i]; }
        esp_codec_dev_write(spk, st, take * 4);
        done += take;
    }
}

// ------------------------------------------------------------------ events from the bridge
void handle_json(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(root, "lang"));
    if (!type) { cJSON_Delete(root); return; }
    if (!strcmp(type, "partial") && text) {
        set_hearing(text);
    } else if (!strcmp(type, "segment") && text && lang) {
        ESP_LOGI(TAG, "[%s] %s", lang, text);
        set_hearing("");
        if (!strcmp(lang, "es")) set_pair(them_latest, them_prev, text); else set_pair(you_latest, you_prev, text);
    } else if (!strcmp(type, "translation") && text && lang) {
        ESP_LOGI(TAG, "  -> [%s] %s", lang, text);
        if (!strcmp(lang, "es")) set_pair(them_latest, them_prev, text); else set_pair(you_latest, you_prev, text);
    } else if (!strcmp(type, "audio")) {
        cJSON *b = cJSON_GetObjectItem(root, "bytes");
        audio_expected = cJSON_IsNumber(b) ? b->valueint : 0;
        audio_len = 0;
        if (audio_buf) { free(audio_buf); audio_buf = nullptr; }
        if (audio_expected > 0 && audio_expected < 4 * 1024 * 1024) audio_buf = (uint8_t *)heap_caps_malloc(audio_expected, MALLOC_CAP_SPIRAM);
    } else if (!strcmp(type, "status") && text) {
        set_state(text);
    }
    cJSON_Delete(root);
}

void ws_event(void *, esp_event_base_t, int32_t id, void *data)
{
    auto *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "bridge connected");
        set_state("CONNECTED");
        esp_websocket_client_send_text(ws, "{\"type\":\"config\",\"a\":\"es\",\"b\":\"en\",\"speak\":true}", 52, pdMS_TO_TICKS(1000));
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        set_state("BRIDGE LOST");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x01) {                       // text: one JSON event, may arrive in fragments
            static char acc[2048]; static int acc_len = 0;
            if (e->payload_offset == 0) acc_len = 0;
            int room = (int)sizeof acc - 1 - acc_len;
            int n = e->data_len < room ? e->data_len : room;
            memcpy(acc + acc_len, e->data_ptr, n); acc_len += n;
            if (e->payload_offset + e->data_len >= e->payload_len) { acc[acc_len] = 0; handle_json(acc, acc_len); }
        } else if (e->op_code == 0x02 && audio_buf) {   // binary: translation audio, fragments by offset
            if (e->payload_offset + e->data_len <= audio_expected) {
                memcpy(audio_buf + e->payload_offset, e->data_ptr, e->data_len);
                audio_len = e->payload_offset + e->data_len;
                if (audio_len >= audio_expected) {
                    // Hand the finished clip to the playback task; never play here, this is the
                    // socket's own task and blocking it drops microphone frames.
                    Clip c = {audio_buf, audio_len};
                    if (!speak || xQueueSend(clips, &c, 0) != pdTRUE) free(audio_buf);
                    audio_buf = nullptr; audio_expected = 0;
                }
            }
        }
        break;
    default: break;
    }
}

// ------------------------------------------------------------------ playback task
void play_task(void *)
{
    Clip c;
    while (active) {
        if (xQueueReceive(clips, &c, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        play_pcm16k(c.pcm, c.len);
        free(c.pcm);
    }
    play_task_h = nullptr;
    vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ microphone
void mic_task(void *)
{
    const int frames = RATE * FRAME_MS / 1000;
    int16_t *raw = (int16_t *)heap_caps_malloc(frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);   // stereo capture
    int16_t *mono = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    while (active) {
        if (!listening || !mic) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (esp_codec_dev_read(mic, raw, frames * 2 * sizeof(int16_t)) != 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        bool muted = esp_timer_get_time() < mute_until_us;
        for (int i = 0; i < frames; i++) mono[i] = muted ? 0 : (int16_t)((raw[2 * i] + raw[2 * i + 1]) / 2);
        if (ws && esp_websocket_client_is_connected(ws))
            esp_websocket_client_send_bin(ws, (const char *)mono, frames * sizeof(int16_t), pdMS_TO_TICKS(500));
    }
    free(raw); free(mono);
    mic_task_h = nullptr;
    vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ lifecycle
/** Connect once the network is up; at boot this app can be opened before WiFi exists. */
void connect_task(void *)
{
    net::wait_connected();
    if (!active) { vTaskDelete(nullptr); return; }
    char uri[160];
    snprintf(uri, sizeof uri, "ws://%s:%d/translate/live", bridge::resolved_host(), bridge::port());
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = 4096;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 5000;
    ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_websocket_client_start(ws);
    ESP_LOGI(TAG, "connecting %s", uri);
    vTaskDelete(nullptr);
}

void on_enter()
{
    active = true;
    stream::stop();                          // the radio releases the codec
    spk_open = false;
    if (!mic) mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16; fs.channel = 2;
    fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    fs.sample_rate = RATE;
    if (esp_codec_dev_open(mic, &fs) != 0) ESP_LOGE(TAG, "mic open failed");
    esp_codec_dev_set_in_gain(mic, 30.0f);
    set_state(net::connected() ? "CONNECTING" : "WAITING FOR WIFI");
    xTaskCreatePinnedToCore(connect_task, "ws_connect", 6 * 1024, nullptr, 4, nullptr, 0);
    if (!clips) clips = xQueueCreate(4, sizeof(Clip));
    if (!mic_task_h) xTaskCreatePinnedToCore(mic_task, "mic", 6 * 1024, nullptr, 6, &mic_task_h, 0);
    if (!play_task_h) xTaskCreatePinnedToCore(play_task, "tts_play", 6 * 1024, nullptr, 5, &play_task_h, 1);
}

void on_exit()
{
    active = false;
    listening = false;
    if (ws) { esp_websocket_client_stop(ws); esp_websocket_client_destroy(ws); ws = nullptr; }
    for (int i = 0; i < 30 && (mic_task_h || play_task_h); i++) vTaskDelay(pdMS_TO_TICKS(20));
    Clip c; while (clips && xQueueReceive(clips, &c, 0) == pdTRUE) free(c.pcm);
    if (mic) esp_codec_dev_close(mic);
    if (spk_open) { esp_codec_dev_close((esp_codec_dev_handle_t)stream::speaker()); spk_open = false; }
}

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 1: listening = !listening; style_key(key_listen, listening); lv_label_set_text(lbl_state, listening ? "LISTENING" : "PAUSED"); break;
    case 2: translate::toggle_flip(); break;
    case 3: speak = !speak; style_key(key_speak, speak); break;
    }
}

lv_obj_t *build()
{
    using namespace theme;
    if (scr) return scr;
    scr = screen();
    topbar::create(scr, "translate");
    const int top = PAD + topbar::HEIGHT + GAP;
    const int keys_h = 56;
    const int ph = (H - top - PAD - GAP - keys_h - GAP) / 2;
    const int w = W - 2 * PAD;

    panel_them = panel(scr, PAD, top, w, ph);
    module_label(panel_them, "01", "ELLOS . ESPAÑOL");
    them_latest = label(panel_them, "", &familjen_medium_24, INK);
    lv_label_set_long_mode(them_latest, LV_LABEL_LONG_WRAP); lv_obj_set_width(them_latest, w - 36);
    lv_obj_align(them_latest, LV_ALIGN_TOP_LEFT, 0, 34);
    them_prev = label(panel_them, "", &familjen_semibold_18, MID);
    lv_label_set_long_mode(them_prev, LV_LABEL_LONG_DOT); lv_obj_set_width(them_prev, w - 36);
    lv_obj_align(them_prev, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_transform_pivot_x(panel_them, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(panel_them, ph / 2, 0);

    panel_you = panel(scr, PAD, top + ph + GAP, w, ph);
    module_label(panel_you, "02", "YOU . ENGLISH");
    you_latest = label(panel_you, "", &familjen_medium_24, INK);
    lv_label_set_long_mode(you_latest, LV_LABEL_LONG_WRAP); lv_obj_set_width(you_latest, w - 36);
    lv_obj_align(you_latest, LV_ALIGN_TOP_LEFT, 0, 34);
    you_prev = label(panel_you, "", &familjen_semibold_18, MID);
    lv_label_set_long_mode(you_prev, LV_LABEL_LONG_DOT); lv_obj_set_width(you_prev, w - 36);
    lv_obj_align(you_prev, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, PAD, H - PAD - keys_h); lv_obj_set_size(row, w, keys_h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    key_listen = keycap(row, "LISTEN", 110, 48, false, key_cb, (void *)1);
    key_flip   = keycap(row, "FLIP", 90, 48, false, key_cb, (void *)2);
    key_speak  = keycap(row, "VOICE", 90, 48, true, key_cb, (void *)3);
    lbl_state  = label(row, "IDLE", &jbmono_14, MID);
    lbl_hear   = label(row, "", &familjen_semibold_18, MID);
    lv_label_set_long_mode(lbl_hear, LV_LABEL_LONG_DOT); lv_obj_set_width(lbl_hear, w - 110 - 90 - 90 - 120 - 5 * 12);
    return scr;
}

const char *status() { return listening ? "LISTENING" : ""; }

} // namespace

const App translate_app = { "translate", "translate", "Live Spanish and English at the table", LV_SYMBOL_GPS, build, on_enter, on_exit, status };

namespace translate {

void set_listening(bool on)
{
    listening = on;
    theme::lock(); style_key(key_listen, on); lv_label_set_text(lbl_state, on ? "LISTENING" : "PAUSED"); theme::unlock();
}

void toggle_flip()
{
    flipped = !flipped;
    lv_obj_set_style_transform_rotation(panel_them, flipped ? 1800 : 0, 0);
    style_key(key_flip, flipped);
}

void register_console()
{
    console::add("listen", [](const char *a, int) { set_listening(!*a || !strcmp(a, "on")); }, "listen [on|off]: stream the microphone to the bridge");
    console::add("flip", [](const char *, int) { theme::lock(); toggle_flip(); theme::unlock(); }, "flip the far panel for the person opposite");
}

}
