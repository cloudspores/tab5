#include "stations.h"
#include "settings.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "stations";

namespace stations {

static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p) { heap_caps_free(p); }
static void  use_psram_for_json()
{
    static bool done = false;
    if (done) return;
    cJSON_Hooks h = {psram_malloc, psram_free};
    cJSON_InitHooks(&h);
    done = true;
}

static const std::vector<Station> BUILTIN = {
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3",        "MP3", 128, "US"},
    {"Radio Paradise",      "http://stream.radioparadise.com/mp3-128",           "MP3", 128, "US"},
    {"FIP",                 "http://icecast.radiofrance.fr/fip-midfi.mp3",       "MP3", 128, "FR"},
    {"KEXP 90.3",           "http://kexp-mp3-128.streamguys1.com/kexp128.mp3",   "MP3", 128, "US"},
    {"SomaFM Lush",         "http://ice1.somafm.com/lush-128-mp3",               "MP3", 128, "US"},
    {"SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3",        "MP3", 128, "US"},
    {"Jazz24",              "http://live.wostreaming.net/direct/ppm-jazz24mp3-ibc1", "MP3", 128, "US"},
    {"Radio Swiss Jazz",    "http://stream.srg-ssr.ch/m/rsj/mp3_128",            "MP3", 128, "CH"},
};

const std::vector<Station> &builtin() { return BUILTIN; }

static Station presets_[PRESET_SLOTS];

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1); dst[cap - 1] = 0;
}

// Minimal URL encoder for the query string.
static void url_encode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else o += snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = 0;
}

bool search(const char *name, const char *countrycode, std::vector<Station> &out, int limit)
{
    char q[160] = "";
    if (name && *name) url_encode(name, q, sizeof q);
    char url[400];
    snprintf(url, sizeof url,
             "https://de1.api.radio-browser.info/json/stations/search?limit=%d&order=clickcount&reverse=true&hidebroken=true%s%s%s%s",
             limit, q[0] ? "&name=" : "", q, (countrycode && *countrycode) ? "&countrycode=" : "", countrycode ? countrycode : "");
    ESP_LOGI(TAG, "GET %s", url);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "Tab5Radio/0.1";
    cfg.buffer_size = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    const int CAP = 96 * 1024;
    char *body = (char *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    int len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            while (len < CAP - 1) {
                int n = esp_http_client_read(c, body + len, CAP - 1 - len);
                if (n <= 0) break;
                len += n;
            }
            body[len] = 0;
            ok = true;
        } else {
            ESP_LOGE(TAG, "http status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "open failed");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) { free(body); return false; }

    out.clear();
    use_psram_for_json();
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsArray(root)) { ESP_LOGE(TAG, "bad json"); if (root) cJSON_Delete(root); return false; }
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(it, "url_resolved"));
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        const char *codec = cJSON_GetStringValue(cJSON_GetObjectItem(it, "codec"));
        if (!u || !n || !*u) continue;
        if (strstr(u, ".m3u8") || strstr(u, ".m3u") || strstr(u, ".pls")) continue;   // playlists/HLS not supported
        if (codec && !(strstr(codec, "MP3") || strstr(codec, "AAC"))) continue;
        Station s = {};
        copy_str(s.name, sizeof s.name, n);
        copy_str(s.url, sizeof s.url, u);
        copy_str(s.codec, sizeof s.codec, codec && strstr(codec, "AAC") ? "AAC" : "MP3");
        cJSON *br = cJSON_GetObjectItem(it, "bitrate");
        s.bitrate = cJSON_IsNumber(br) ? br->valueint : 0;
        copy_str(s.country, sizeof s.country, cJSON_GetStringValue(cJSON_GetObjectItem(it, "countrycode")));
        // trim trailing whitespace in names
        for (int i = strlen(s.name) - 1; i >= 0 && (s.name[i] == ' ' || s.name[i] == '\n'); i--) s.name[i] = 0;
        out.push_back(s);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d stations", (int)out.size());
    return true;
}

const Station &preset(int slot) { return presets_[slot < 0 ? 0 : slot >= PRESET_SLOTS ? PRESET_SLOTS - 1 : slot]; }

void set_preset(int slot, const Station &s)
{
    if (slot < 0 || slot >= PRESET_SLOTS) return;
    presets_[slot] = s;
    char key[8]; snprintf(key, sizeof key, "p%d", slot);
    settings::set_blob(key, &s, sizeof s);
}

void load_presets()
{
    for (int i = 0; i < PRESET_SLOTS; i++) {
        char key[8]; snprintf(key, sizeof key, "p%d", i);
        if (!settings::get_blob(key, &presets_[i], sizeof(Station))) memset(&presets_[i], 0, sizeof(Station));
    }
    // First boot: seed the presets from the built-in list so the module isn't empty.
    bool any = false;
    for (int i = 0; i < PRESET_SLOTS; i++) any |= presets_[i].name[0] != 0;
    if (!any) for (int i = 0; i < PRESET_SLOTS && i < (int)BUILTIN.size(); i++) set_preset(i, BUILTIN[i]);
}

bool load_last(Station &s) { return settings::get_blob("last", &s, sizeof s) && s.name[0]; }
void save_last(const Station &s) { settings::set_blob("last", &s, sizeof s); }

} // namespace stations
