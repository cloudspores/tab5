#include "c6_update.h"
#include "ui.h"
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "eh_host_core.h"
#include "eh_host_sys.h"
#include "eh_host_cp_ota.h"

static const char *TAG = "c6";

extern const uint8_t c6_fw_start[] asm("_binary_c6_fw_bin_start");
extern const uint8_t c6_fw_end[]   asm("_binary_c6_fw_bin_end");

namespace c6 {

void update_if_needed()
{
#ifdef NO_C6_FW
    ESP_LOGW(TAG, "built without the C6 image; skipping co-processor update");
    return;
#endif
    eh_host_coprocessor_fwver_t v = {};
    if (eh_host_sys_get_cp_fw_version(&v) != ESP_OK) {
        ESP_LOGW(TAG, "could not read co-processor version, skipping update");
        return;
    }
    ESP_LOGI(TAG, "co-processor firmware %lu.%lu.%lu", (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
    if (v.major1 >= 2) return;

    const size_t total = c6_fw_end - c6_fw_start;
    ESP_LOGW(TAG, "legacy co-processor firmware, updating from embedded image (%u bytes)", (unsigned)total);
    ui::set_status("C6 UPDATE 0%");

    esp_err_t r = eh_host_cp_ota_begin();
    if (r != ESP_OK) { ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(r)); ui::set_status("C6 UPDATE FAILED"); return; }

    const size_t CHUNK = 1500;
    size_t done = 0; int last_pct = -1;
    while (done < total) {
        size_t n = total - done < CHUNK ? total - done : CHUNK;
        r = eh_host_cp_ota_write(c6_fw_start + done, n);
        if (r != ESP_OK) { ESP_LOGE(TAG, "ota write failed at %u: %s", (unsigned)done, esp_err_to_name(r)); ui::set_status("C6 UPDATE FAILED"); return; }
        done += n;
        int pct = (int)(done * 100 / total);
        if (pct != last_pct && pct % 5 == 0) {
            char s[32]; snprintf(s, sizeof s, "C6 UPDATE %d%%", pct);
            ui::set_status(s); ESP_LOGI(TAG, "%d%%", pct); last_pct = pct;
        }
    }
    r = eh_host_cp_ota_end();
    if (r != ESP_OK) { ESP_LOGE(TAG, "ota end failed: %s", esp_err_to_name(r)); ui::set_status("C6 UPDATE FAILED"); return; }
    ESP_LOGI(TAG, "ota complete");

    // Activate exists on 2.6+ co-processors only; a legacy one boots the new slot after reset.
    r = eh_host_cp_ota_activate();
    ESP_LOGI(TAG, "activate: %s", esp_err_to_name(r));

    ui::set_status("C6 UPDATED, RESTART");
    ESP_LOGW(TAG, "restarting host to resync with the new co-processor firmware");
    eh_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

} // namespace c6
