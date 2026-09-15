#include "bridge.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "secrets.h"
#include "settings.h"
#include "mdns.h"
#include "esp_netif_ip_addr.h"

static const char *TAG = "bridge";

namespace {

/**
 * The bridge runs on a laptop whose DHCP address changes; BRIDGE_HOST is normally its Bonjour name
 * ("name.local"). Resolve it with mDNS and cache the answer; a plain IP or hostname passes through.
 */
const char *configured_host()
{
    static char h[80] = "";
    if (!h[0]) {
        if (!settings::get_str("bridge", h, sizeof h) || !h[0]) {
            strlcpy(h, BRIDGE_HOST, sizeof h);
            if (h[0]) settings::set_str("bridge", h);
        }
    }
    return h;
}

const char *host()
{
    static char cached[40] = "";
    static int64_t resolved_at = 0;
    const char *h = configured_host();
    size_t n = strlen(h);
    if (n < 7 || strcmp(h + n - 6, ".local") != 0) return h;
    int64_t now = esp_timer_get_time();
    if (cached[0] && now - resolved_at < 10LL * 60 * 1000000) return cached;   // re-resolve every 10 min
    char name[64];
    strlcpy(name, h, sizeof name);
    name[n - 6] = 0;                                    // mdns wants the name without ".local"
    esp_ip4_addr_t addr = {};
    if (mdns_query_a(name, 3000, &addr) == ESP_OK) {
        snprintf(cached, sizeof cached, IPSTR, IP2STR(&addr));
        resolved_at = now;
        ESP_LOGI(TAG, "%s -> %s", h, cached);
        return cached;
    }
    ESP_LOGW(TAG, "mDNS could not resolve %s", h);
    return cached[0] ? cached : h;
}

// Perform a request, return body (empty on failure). Small responses only.
bool request(const char *method, const char *path, const char *json_body, std::string &out)
{
    char url[256];
    snprintf(url, sizeof url, "http://%s:%d%s", host(), BRIDGE_PORT, path);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;   // first /sonos/rooms after a bridge start includes discovery
    cfg.method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    if (json_body) esp_http_client_set_header(c, "Content-Type", "application/json");
    int blen = json_body ? strlen(json_body) : 0;
    bool ok = false;
    out.clear();
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (blen) esp_http_client_write(c, json_body, blen);
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) out.append(buf, n);
        ok = status >= 200 && status < 300;
        if (!ok) ESP_LOGW(TAG, "%s %s -> %d", method, path, status);
    } else {
        ESP_LOGW(TAG, "%s %s: connect failed", method, path);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

} // namespace

namespace bridge {

const char *resolved_host() { return host(); }
int port() { return BRIDGE_PORT; }

void set_host(const char *h) { settings::set_str("bridge", h); ESP_LOGI(TAG, "bridge host set to %s (takes effect after restart)", h); }

bool reachable()
{
    std::string body;
    return request("GET", "/health", nullptr, body);
}

bool sonos_rooms(std::vector<std::string> &names)
{
    std::string body;
    if (!request("GET", "/sonos/rooms", nullptr, body)) return false;
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return false;
    names.clear();
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        if (n) names.push_back(n);
    }
    cJSON_Delete(root);
    return true;
}

bool sonos_play_url(const char *room, const char *url, const char *title)
{
    char r[96], u[400], t[160], body[720];
    json_escape(room, r, sizeof r); json_escape(url, u, sizeof u); json_escape(title, t, sizeof t);
    snprintf(body, sizeof body, "{\"room\":\"%s\",\"url\":\"%s\",\"title\":\"%s\"}", r, u, t);
    std::string resp;
    bool ok = request("POST", "/sonos/play_url", body, resp);
    ESP_LOGI(TAG, "play_url: %s", resp.c_str());
    return ok;
}

bool synth_relay(const char *room)
{
    char r[96], body[160];
    json_escape(room, r, sizeof r);
    snprintf(body, sizeof body, "{\"room\":\"%s\"}", r);
    std::string resp;
    bool ok = request("POST", "/synth/sonos", body, resp);
    ESP_LOGI(TAG, "synth relay: %s", resp.c_str());
    return ok;
}
bool sonos_stop(const char *room)
{
    char r[96], body[160];
    json_escape(room, r, sizeof r);
    snprintf(body, sizeof body, "{\"room\":\"%s\",\"action\":\"stop\"}", r);
    std::string resp;
    return request("POST", "/sonos/cmd", body, resp);
}

bool sonos_volume(const char *room, int volume)
{
    char r[96], body[160];
    json_escape(room, r, sizeof r);
    snprintf(body, sizeof body, "{\"room\":\"%s\",\"volume\":%d}", r, volume);
    std::string resp;
    return request("POST", "/sonos/volume", body, resp);
}

bool sonos_now_playing(const char *room, std::string &title, std::string &artist, bool &playing)
{
    char path[160], r[96] = "";
    // percent-encode spaces only; room names are simple
    for (const char *p = room; *p && strlen(r) < sizeof r - 4; p++) {
        if (*p == ' ') strcat(r, "%20"); else { size_t l = strlen(r); r[l] = *p; r[l + 1] = 0; }
    }
    snprintf(path, sizeof path, "/sonos/state?room=%s", r);
    std::string body;
    if (!request("GET", path, nullptr, body)) return false;
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) return false;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(root, "artist"));
    title = t ? t : ""; artist = a ? a : "";
    playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "playing"));
    cJSON_Delete(root);
    return true;
}

} // namespace bridge
