#include "stream.h"
#include "radio_ui.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

static const char *TAG = "stream";

namespace {

constexpr int HTTP_CHUNK   = 4096;
constexpr int IN_BUF_SIZE  = 64 * 1024;   // compressed input staging
constexpr int OUT_BUF_INIT = 16 * 1024;   // decoded PCM
constexpr int META_MAX     = 4096;        // ICY metadata block max (255*16 + 1)

esp_codec_dev_handle_t spk = nullptr;
TaskHandle_t           task = nullptr;
stream::Station        current{};
volatile int           generation = 0;    // bumped on every play()/stop()
volatile bool          is_playing = false;
bool                   is_muted = false;
int                    volume = 60;

struct Icy {
    int  metaint = 0;
    int  countdown = 0;     // audio bytes until next metadata block
    int  meta_len = -1;     // -1: expecting length byte
    int  meta_pos = 0;
    char meta[META_MAX + 1];
};

struct Player {
    int gen;
    esp_audio_simple_dec_handle_t dec = nullptr;
    uint8_t *in = nullptr;  int in_len = 0;
    uint8_t *out = nullptr; int out_cap = 0;
    bool codec_open = false;
    int  sample_rate = 0, channels = 0, bitrate = 0;
    const char *codec_name = "MP3";
    Icy icy;
};

bool alive(const Player &p) { return p.gen == generation; }

void parse_stream_title(const char *meta)
{
    const char *k = strstr(meta, "StreamTitle='");
    if (!k) return;
    k += strlen("StreamTitle='");
    const char *e = strstr(k, "';");
    size_t n = e ? (size_t)(e - k) : strlen(k);
    char title[256];
    if (n >= sizeof title) n = sizeof title - 1;
    memcpy(title, k, n); title[n] = 0;
    ESP_LOGI(TAG, "now playing: %s", title);
    ui::set_title(title);
}

void open_codec(Player &p)
{
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2;                       // we always emit stereo
    fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    fs.sample_rate = p.sample_rate;
    fs.mclk_multiple = 0;
    if (p.codec_open) esp_codec_dev_close(spk);
    int r = esp_codec_dev_open(spk, &fs);
    if (r != 0) { ESP_LOGE(TAG, "codec open failed %d", r); return; }
    esp_codec_dev_set_out_vol(spk, volume);
    esp_codec_dev_set_out_mute(spk, is_muted);
    p.codec_open = true;
    ESP_LOGI(TAG, "codec open: %d Hz, %d ch in, %d kbps", p.sample_rate, p.channels, p.bitrate / 1000);
    ui::set_format(p.codec_name, p.bitrate / 1000, p.sample_rate, p.channels);
}

// Write one decoded frame: upmix mono if needed, feed the meter, push to codec.
void emit_pcm(Player &p, uint8_t *pcm, int len)
{
    int16_t *s = (int16_t *)pcm;
    int n = len / 2;
    int peak = 0;
    for (int i = 0; i < n; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > peak) peak = v; }
    // dBFS scale: -48 dB -> 0, 0 dB -> 100, so normal programme material sits mid-meter
    float db = peak <= 0 ? -96.0f : 20.0f * log10f((float)peak / 32767.0f);
    int level = (int)((db + 48.0f) * (100.0f / 48.0f));
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    ui::push_level((uint8_t)level);
    static int dbg = 0;
    if (++dbg % 500 == 0) ESP_LOGI(TAG, "meter peak %d (%.1f dBFS) level %d", peak, db, level);

    if (p.channels == 1) {
        static int16_t st[8192];
        int done = 0;
        while (done < n) {
            int take = n - done; if (take > 4096) take = 4096;
            for (int i = 0; i < take; i++) { st[2 * i] = s[done + i]; st[2 * i + 1] = s[done + i]; }
            esp_codec_dev_write(spk, st, take * 4);
            done += take;
        }
    } else {
        esp_codec_dev_write(spk, pcm, len);
    }
}

void decode_pending(Player &p)
{
    while (p.in_len > 0 && alive(p)) {
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = p.in; raw.len = p.in_len; raw.eos = false;
        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = p.out; frame.len = p.out_cap;
        esp_audio_err_t r = esp_audio_simple_dec_process(p.dec, &raw, &frame);
        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > (uint32_t)p.out_cap) {
            p.out = (uint8_t *)heap_caps_realloc(p.out, frame.needed_size, MALLOC_CAP_SPIRAM);
            p.out_cap = frame.needed_size;
            continue;
        }
        if (r == ESP_AUDIO_ERR_DATA_LACK) break;             // wait for more input
        if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_CONTINUE) {
            ESP_LOGW(TAG, "decode err %d, skipping", r);
            raw.consumed = raw.consumed ? raw.consumed : 1;   // drop a byte and resync
        }
        if (frame.decoded_size > 0) {
            if (!p.codec_open) {
                esp_audio_simple_dec_info_t info = {};
                esp_audio_simple_dec_get_info(p.dec, &info);
                p.sample_rate = info.sample_rate; p.channels = info.channel; p.bitrate = info.bitrate;
                if (p.sample_rate > 0) open_codec(p);
            }
            if (p.codec_open) emit_pcm(p, frame.buffer, frame.decoded_size);
        }
        if (raw.consumed == 0) break;
        p.in_len -= raw.consumed;
        if (p.in_len > 0) memmove(p.in, p.in + raw.consumed, p.in_len);
    }
}

void feed_audio(Player &p, const uint8_t *data, int len)
{
    while (len > 0 && alive(p)) {
        int room = IN_BUF_SIZE - p.in_len;
        if (room <= 0) { decode_pending(p); if (IN_BUF_SIZE - p.in_len <= 0) { ESP_LOGW(TAG, "input overrun"); p.in_len = 0; } continue; }
        int take = len < room ? len : room;
        memcpy(p.in + p.in_len, data, take);
        p.in_len += take; data += take; len -= take;
        decode_pending(p);
    }
}

// Split an ICY stream chunk into audio bytes and metadata blocks.
void feed_icy(Player &p, const uint8_t *data, int len)
{
    Icy &ic = p.icy;
    if (ic.metaint <= 0) { feed_audio(p, data, len); return; }
    while (len > 0) {
        if (ic.countdown > 0) {
            int take = len < ic.countdown ? len : ic.countdown;
            feed_audio(p, data, take);
            data += take; len -= take; ic.countdown -= take;
            continue;
        }
        if (ic.meta_len < 0) {                 // length byte
            ic.meta_len = (*data) * 16; ic.meta_pos = 0;
            data++; len--;
            if (ic.meta_len == 0) { ic.meta_len = -1; ic.countdown = ic.metaint; }
            continue;
        }
        int take = len < (ic.meta_len - ic.meta_pos) ? len : (ic.meta_len - ic.meta_pos);
        if (ic.meta_pos + take <= META_MAX) memcpy(ic.meta + ic.meta_pos, data, take);
        ic.meta_pos += take; data += take; len -= take;
        if (ic.meta_pos >= ic.meta_len) {
            ic.meta[ic.meta_len > META_MAX ? META_MAX : ic.meta_len] = 0;
            parse_stream_title(ic.meta);
            ic.meta_len = -1; ic.countdown = ic.metaint;
        }
    }
}

// Response headers are only delivered through the client's event callback.
struct Headers { int metaint = 0; int br = 0; char content_type[64] = ""; char name[64] = ""; };

esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->header_key || !evt->header_value) return ESP_OK;
    auto *h = (Headers *)evt->user_data;
    if (!strcasecmp(evt->header_key, "icy-metaint")) h->metaint = atoi(evt->header_value);
    else if (!strcasecmp(evt->header_key, "icy-br")) h->br = atoi(evt->header_value);
    else if (!strcasecmp(evt->header_key, "icy-name")) strlcpy(h->name, evt->header_value, sizeof h->name);
    else if (!strcasecmp(evt->header_key, "Content-Type")) strlcpy(h->content_type, evt->header_value, sizeof h->content_type);
    return ESP_OK;
}

bool connect(Player &p, esp_http_client_handle_t &client, const char *url)
{
    static Headers hdr;
    hdr = Headers{};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event;
    cfg.user_data = &hdr;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "Tab5Radio/0.1";
    client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) { ESP_LOGE(TAG, "open failed"); return false; }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            ESP_LOGI(TAG, "redirect %d", status);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            hdr = Headers{};
            continue;
        }
        if (status != 200) { ESP_LOGE(TAG, "http status %d", status); return false; }
        if (hdr.metaint > 0) { p.icy.metaint = hdr.metaint; p.icy.countdown = hdr.metaint; p.icy.meta_len = -1; }
        if (hdr.br > 0) p.bitrate = hdr.br * 1000;
        if (hdr.name[0]) ESP_LOGI(TAG, "icy-name: %s", hdr.name);
        const char *ct = hdr.content_type;
        bool aac = strstr(ct, "aac") != nullptr || strstr(ct, "mp4") != nullptr;
        p.codec_name = aac ? "AAC" : "MP3";
        esp_audio_simple_dec_cfg_t dcfg = {};
        dcfg.dec_type = aac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        if (esp_audio_simple_dec_open(&dcfg, &p.dec) != ESP_AUDIO_ERR_OK) { ESP_LOGE(TAG, "decoder open failed"); return false; }
        ESP_LOGI(TAG, "connected: %s, metaint %d, type %s", ct, p.icy.metaint, p.codec_name);
        return true;
    }
    return false;
}

void player_task(void *)
{
    Player p;
    p.gen = generation;
    p.in  = (uint8_t *)heap_caps_malloc(IN_BUF_SIZE, MALLOC_CAP_SPIRAM);
    p.out = (uint8_t *)heap_caps_malloc(OUT_BUF_INIT, MALLOC_CAP_SPIRAM);
    p.out_cap = OUT_BUF_INIT;
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(HTTP_CHUNK, MALLOC_CAP_SPIRAM);

    while (alive(p)) {
        esp_http_client_handle_t client = nullptr;
        p.icy = Icy{}; p.in_len = 0;
        ui::set_status("CONNECTING");
        if (connect(p, client, current.url)) {
            is_playing = true;
            ui::set_status("ON AIR");
            while (alive(p)) {
                int n = esp_http_client_read(client, (char *)chunk, HTTP_CHUNK);
                if (n < 0) { ESP_LOGW(TAG, "read error"); break; }
                if (n == 0) { ESP_LOGW(TAG, "stream ended"); break; }
                feed_icy(p, chunk, n);
            }
        } else {
            ui::set_status("STREAM FAILED");
        }
        is_playing = false;
        if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
        if (p.dec) { esp_audio_simple_dec_close(p.dec); p.dec = nullptr; }
        if (p.codec_open) { esp_codec_dev_close(spk); p.codec_open = false; }
        if (!alive(p)) break;
        ui::set_status("RECONNECT");
        for (int i = 0; i < 30 && alive(p); i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    free(chunk); free(p.in); free(p.out);
    ESP_LOGI(TAG, "player task exit");
    task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

namespace stream {

void init()
{
    spk = bsp_audio_codec_speaker_init();
    if (!spk) ESP_LOGE(TAG, "speaker init failed");
    esp_audio_dec_register_default();          // MP3/AAC/... codec implementations
    esp_audio_simple_dec_register_default();   // container/frame parsers on top
}

void play(const Station &s)
{
    stop();
    current = s;
    ui::set_station(s.name);
    ui::set_title("");
    xTaskCreatePinnedToCore(player_task, "player", 12 * 1024, nullptr, 6, &task, 0);
}

void stop()
{
    generation = generation + 1;
    // Wait for the player task to exit. It owns the codec handle while it runs, so
    // callers (the synth, for one) may only reopen the codec once this returns.
    // A blocked HTTP read can hold the task for its socket timeout, hence the bound.
    int i = 0;
    for (; i < 400 && task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (task) ESP_LOGW(TAG, "player task still running after stop()");
    else if (i) ESP_LOGI(TAG, "player stopped after %d ms", i * 20);
}

bool playing() { return is_playing; }

void set_mute(bool on)
{
    is_muted = on;
    if (spk) esp_codec_dev_set_out_mute(spk, on);
}

bool muted() { return is_muted; }

int volume_percent() { return volume; }
void *speaker() { return spk; }

void set_volume(int percent)
{
    volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (spk) esp_codec_dev_set_out_vol(spk, volume);
}

} // namespace stream
