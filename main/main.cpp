/**
 * @file main.cpp
 * @brief Boot sequence for the Tab5 firmware: display, launcher, apps, network.
 *
 * Everything app-specific lives in its own module; this file only orders the bring-up.
 */
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/i2c_master.h"

#include "settings.h"
#include "theme.h"
#include "launcher.h"
#include "console.h"
#include "net.h"
#include "bridge.h"
#include "update.h"
#include "radio_app.h"
#include "settings_app.h"

static const char *TAG = "main";

/** Placeholder card until the synth lands; the launcher ignores apps without a screen. */
static const App synth_app_placeholder = { "synth", "fm synth", "Six-operator synth, sequencer, songs (coming next)", LV_SYMBOL_LOOP, nullptr, nullptr, nullptr, nullptr };

/** A USB reset does not power-cycle the panel; cycling the LCD and touch rails makes the BSP's probe reliable. */
static void power_cycle_panel()
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_feature_enable(BSP_FEATURE_TOUCH, false);
    bsp_feature_enable(BSP_FEATURE_LCD, false);
    vTaskDelay(pdMS_TO_TICKS(300));
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    vTaskDelay(pdMS_TO_TICKS(300));
}

/** Portrait-native panel rotated to landscape by the PPA engine; full-frame buffers live in PSRAM. */
static void start_display()
{
    bsp_display_cfg_t cfg = {};
    cfg.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.lvgl_port_cfg.task_affinity = 1;      // core 0 stays free for WiFi, hosted and LWIP
    cfg.lvgl_port_cfg.task_stack = 12 * 1024;
    cfg.buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
    cfg.double_buffer = true;
    cfg.flags.buff_dma = true;
    cfg.flags.buff_spiram = true;
    cfg.flags.sw_rotate = true;               // "software" rotation is done by the PPA when enabled
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    theme::lock();
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    theme::unlock();
    bsp_display_backlight_on();
    bsp_display_brightness_set(settings::get_int("light", 50));
}

/** Console verbs that belong to the system rather than an app. */
static void update_task(void *arg)
{
    const bool install = arg != nullptr;
    char v[24] = "", u[256] = "", n[160] = "";
    bool newer = update::check(v, sizeof v, u, sizeof u, n, sizeof n);
    ESP_LOGI(TAG, "running %s, catalog %s: %s", update::running_version(), v[0] ? v : "?", newer ? "UPDATE AVAILABLE" : "up to date");
    if (install && newer) {
        update::install(u, [](int pct) { ESP_LOGI(TAG, "installing %d%%", pct); });   // restarts on success
        ESP_LOGE(TAG, "install failed");
    }
    vTaskDelete(nullptr);
}

static void register_console()
{
    console::add("home",  [](const char *, int) { launcher::home(); }, "launcher home screen");
    console::add("wifi",  [](const char *a, int) {
        char ssid[33] = ""; const char *sp = strchr(a, ' ');
        if (!sp) { ESP_LOGW(TAG, "usage: wifi SSID PASSWORD"); return; }
        strlcpy(ssid, a, sp - a + 1 < (int)sizeof ssid ? sp - a + 1 : sizeof ssid);
        net::set_credentials(ssid, sp + 1);
    }, "wifi SSID PASSWORD (stored in NVS)");
    console::add("bridge", [](const char *a, int) { if (*a) bridge::set_host(a); else ESP_LOGW(TAG, "usage: bridge NAME.local|IP"); }, "bridge HOST (stored in NVS)");
    console::add("restart", [](const char *, int) { ESP_LOGW(TAG, "restart requested"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }, "reboot the device");
    console::add("open",  [](const char *a, int) { launcher::open(*a ? a : "radio"); }, "open APP (radio, settings)");
    console::add("check",   [](const char *, int) { xTaskCreatePinnedToCore(update_task, "update", 8 * 1024, nullptr, 4, nullptr, 0); }, "check the GitHub catalogue for a newer firmware");
    console::add("install", [](const char *, int) { xTaskCreatePinnedToCore(update_task, "update", 8 * 1024, (void *)1, 4, nullptr, 0); }, "install the catalogue firmware if newer (restarts)");
}

extern "C" void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings::init();
    setenv("TZ", "CST6", 1); tzset();     // Costa Rica: UTC-6, no daylight saving

    power_cycle_panel();
    start_display();

    launcher::add(&radio_app);
    launcher::add(&synth_app_placeholder);
    launcher::add(&settings_app);
    launcher::init();

    radio::init();
    console::start();
    register_console();

    // Show the last-used screen before the network is up so the device feels instant.
    char last_app[16] = "radio";
    settings::get_str("app", last_app, sizeof last_app);
    if (!strcmp(last_app, "home")) launcher::home(); else launcher::open(last_app[0] ? last_app : "radio");

    net::start();
    net::wait_connected();
    settings_app_ns::set_info(net::ip(), "");
    radio::start();                        // room discovery and playback continue in the radio's task
    ESP_LOGI(TAG, "boot complete, firmware %s", update::running_version());
}
