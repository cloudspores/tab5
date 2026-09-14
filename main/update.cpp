#include "update.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "update";
static const char *CATALOG_URL = "https://raw.githubusercontent.com/cloudspores/tab5/main/catalog.json";

namespace update {

const char *running_version() { return esp_app_get_description()->version; }

// "0.1.2" -> 102, "1.0.0" -> 10000; good enough for ordering.
static int vnum(const char *v)
{
    int a = 0, b = 0, c = 0;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a * 10000 + b * 100 + c;
}

bool check(char *version, size_t vcap, char *url, size_t ucap, char *notes, size_t ncap)
{
    esp_http_client_config_t cfg = {};
    cfg.url = CATALOG_URL;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "Tab5Radio";
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    std::string body;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        char buf[1024]; int n;
        while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) body.append(buf, n);
        ok = status == 200;
        if (!ok) ESP_LOGW(TAG, "catalog http %d", status);
    } else ESP_LOGW(TAG, "catalog: connect failed");
    esp_http_client_close(c); esp_http_client_cleanup(c);
    if (!ok) return false;
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) { ESP_LOGW(TAG, "catalog: bad json"); return false; }
    cJSON *fw = cJSON_GetObjectItem(root, "firmware");
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(fw, "version"));
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(fw, "url"));
    const char *nt = cJSON_GetStringValue(cJSON_GetObjectItem(fw, "notes"));
    bool newer = v && u && vnum(v) > vnum(running_version());
    if (v) strlcpy(version, v, vcap);
    if (u) strlcpy(url, u, ucap);
    strlcpy(notes, nt ? nt : "", ncap);
    ESP_LOGI(TAG, "running %s, catalog %s -> %s", running_version(), v ? v : "?", newer ? "update available" : "up to date");
    cJSON_Delete(root);
    return newer;
}

bool install(const char *url, void (*progress)(int))
{
    ESP_LOGW(TAG, "installing %s", url);
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = 30000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.user_agent = "Tab5Radio";
    http.keep_alive_enable = true;
    http.buffer_size = 8192;          // GitHub Releases redirect through a very long signed URL
    http.buffer_size_tx = 2048;
    http.max_redirection_count = 5;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    esp_https_ota_handle_t h = nullptr;
    if (esp_https_ota_begin(&ota, &h) != ESP_OK) { ESP_LOGE(TAG, "ota begin failed"); return false; }
    int total = esp_https_ota_get_image_size(h), last = -1;
    esp_err_t r;
    while ((r = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? done * 100 / total : 0;
        if (pct != last && progress) { progress(pct); last = pct; }
    }
    if (r != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "ota failed: %s", esp_err_to_name(r));
        esp_https_ota_abort(h);
        return false;
    }
    if (esp_https_ota_finish(h) != ESP_OK) { ESP_LOGE(TAG, "ota finish failed"); return false; }
    ESP_LOGW(TAG, "update installed, restarting");
    if (progress) progress(100);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return true;
}

}
