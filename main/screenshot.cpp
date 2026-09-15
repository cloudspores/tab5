/**
 * @file screenshot.cpp
 * @brief Console verb `shot`: capture the active LVGL screen and upload it to the bridge.
 *
 * A development aid: the screen is rendered into a PSRAM buffer with lv_snapshot, then
 * POSTed raw (RGB565, 1280x720) to the bridge's /shot endpoint, which saves it to a file on
 * the Spark. `hold on|off` presses the first key found under a name, to photograph pressed
 * states without a finger.
 */
#include "screenshot.h"
#include "console.h"
#include "theme.h"
#include "bridge.h"

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lvgl.h"
#include "draw/snapshot/lv_snapshot.h"

static const char *TAG = "shot";

namespace {

void shot_task(void *)
{
    theme::lock();
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    theme::unlock();
    if (!buf) { ESP_LOGE(TAG, "snapshot failed"); vTaskDelete(nullptr); return; }
    const int w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    char url[160];
    snprintf(url, sizeof url, "http://%s:%d/shot?w=%d&h=%d", bridge::resolved_host(), bridge::port(), w, h);
    esp_http_client_config_t cfg = {};
    cfg.url = url; cfg.method = HTTP_METHOD_POST; cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    esp_err_t err = esp_http_client_open(c, w * h * 2);
    if (err == ESP_OK) {
        for (int y = 0; y < h; y++) esp_http_client_write(c, (const char *)buf->data + y * stride, w * 2);
        esp_http_client_fetch_headers(c);
        ESP_LOGI(TAG, "uploaded %dx%d, http %d", w, h, esp_http_client_get_status_code(c));
    } else ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    lv_draw_buf_destroy(buf);
    vTaskDelete(nullptr);
}

/** Depth-first search for a button whose label reads `text`. */
lv_obj_t *find_key(lv_obj_t *root, const char *text)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(root, i);
        if (lv_obj_check_type(c, &lv_button_class) && lv_obj_get_child_count(c) > 0) {
            lv_obj_t *l = lv_obj_get_child(c, 0);
            if (lv_obj_check_type(l, &lv_label_class) && !strcmp(lv_label_get_text(l), text)) return c;
        }
        if (lv_obj_t *r = find_key(c, text)) return r;
    }
    return nullptr;
}
}

namespace screenshot {
void register_console()
{
    console::add("shot", [](const char *, int) { xTaskCreatePinnedToCore(shot_task, "shot", 8 * 1024, nullptr, 3, nullptr, 0); }, "upload a screenshot to the bridge");
    console::add("hold", [](const char *a, int) {
        char name[32]; const char *sp = strchr(a, ' ');
        if (!sp) { ESP_LOGW(TAG, "hold KEYTEXT on|off"); return; }
        snprintf(name, sizeof name, "%.*s", (int)(sp - a), a);
        theme::lock();
        lv_obj_t *k = find_key(lv_screen_active(), name);
        bool on = !strcmp(sp + 1, "on"), ok = k != nullptr;
        if (k) { if (on) lv_obj_add_state(k, LV_STATE_PRESSED); else lv_obj_remove_state(k, LV_STATE_PRESSED); }
        else ok = theme::dial_press(name, on);
        theme::unlock();
        ESP_LOGI(TAG, "'%s' %s", name, ok ? "toggled" : "not found");
    }, "hold KEYTEXT on|off: press/release a key by its label (for screenshots)");
}
}
