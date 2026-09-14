#include "bridge.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "secrets.h"

static const char *TAG = "bridge";

namespace {

// Perform a request, return body (empty on failure). Small responses only.
bool request(const char *method, const char *path, const char *json_body, std::string &out)
{
    char url[256];
    snprintf(url, sizeof url, "http://%s:%d%s", BRIDGE_HOST, BRIDGE_PORT, path);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 6000;
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
