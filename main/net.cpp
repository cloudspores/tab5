/**
 * @file net.cpp
 * @brief Network bring-up: C6 link, WiFi station with auto-reconnect, SNTP time.
 */
#include "net.h"
#include "c6_update.h"
#include "topbar.h"
#include "secrets.h"
#include "settings.h"

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_hosted.h"
#include "mdns.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "net";

namespace {

EventGroupHandle_t events = nullptr;
constexpr int CONNECTED_BIT = BIT0;
char ip_text[24] = "";

/** The event group may be awaited by apps before start() runs (boot opens the last-used app first). */
EventGroupHandle_t ev()
{
    if (!events) events = xEventGroupCreate();
    return events;
}

/** WiFi/IP event handler: connect on start, reconnect after a pause on loss, record the IP. */
void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(ev(), CONNECTED_BIT);
        ip_text[0] = 0;
        topbar::set_wifi("WIFI RETRY");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        snprintf(ip_text, sizeof ip_text, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", ip_text);
        topbar::set_wifi("WIFI OK");
        xEventGroupSetBits(ev(), CONNECTED_BIT);
    }
}

} // namespace

namespace net {

void start()
{
    // The C6's power enable sits behind the second IO expander; hosted must not talk to it before
    // that, which is why the hosted auto-init constructor is disabled in sdkconfig.defaults.
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    vTaskDelay(pdMS_TO_TICKS(200));
    topbar::set_wifi("C6 LINK");
    if (esp_hosted_init() != 0) ESP_LOGE(TAG, "esp_hosted_init failed");
    if (esp_hosted_connect_to_slave() != 0) ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed");
    c6::update_if_needed();   // one-time: replaces legacy C6 firmware, restarts the host if it does

    ev();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(mdns_init());          // resolves the bridge's Bonjour name; also advertises "tab5.local"
    mdns_hostname_set("tab5");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));

    // Credentials live in NVS so firmware installed over the air (built without secrets.h) keeps
    // working. A locally built image seeds them from secrets.h on first boot; the console can change them.
    char ssid[33] = "", pass[65] = "";
    if (!settings::get_str("wifi_ssid", ssid, sizeof ssid) || !ssid[0]) {
        strlcpy(ssid, WIFI_SSID, sizeof ssid);
        strlcpy(pass, WIFI_PASS, sizeof pass);
        if (ssid[0]) { settings::set_str("wifi_ssid", ssid); settings::set_str("wifi_pass", pass); ESP_LOGI(TAG, "seeded WiFi credentials from build"); }
    } else {
        settings::get_str("wifi_pass", pass, sizeof pass);
    }
    if (!ssid[0]) ESP_LOGE(TAG, "no WiFi credentials: use the console: wifi SSID PASSWORD");
    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    topbar::set_wifi("WIFI JOIN");
    ESP_ERROR_CHECK(esp_wifi_start());

    // Time of day for the clock; the zone is set by main (Costa Rica, no daylight saving).
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void set_credentials(const char *ssid, const char *pass)
{
    settings::set_str("wifi_ssid", ssid);
    settings::set_str("wifi_pass", pass);
    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi credentials updated for %s", ssid);
}

void wait_connected() { xEventGroupWaitBits(ev(), CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY); }
bool connected() { return (xEventGroupGetBits(ev()) & CONNECTED_BIT) != 0; }
const char *ip() { return ip_text; }

}
