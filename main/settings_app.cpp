#include "settings_app.h"
#include "theme.h"
#include "topbar.h"
#include "update.h"
#include "settings.h"
#include "bridge.h"
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "settings_app";

namespace {
lv_obj_t *scr = nullptr, *lbl_ip, *lbl_bridge, *lbl_fw, *lbl_update, *lbl_light, *key_install;
char ip_text[40] = "--", bridge_text[40] = "--";
char upd_version[24], upd_url[256], upd_notes[160]; bool upd_available = false;
int light = 50;

void set_text(lv_obj_t *l, const char *t) { theme::lock(); lv_label_set_text(l, t); theme::unlock(); }

void check_task(void *)
{
    set_text(lbl_update, "CHECKING...");
    upd_available = update::check(upd_version, sizeof upd_version, upd_url, sizeof upd_url, upd_notes, sizeof upd_notes);
    char buf[256];
    if (upd_available) snprintf(buf, sizeof buf, "UPDATE %.20s AVAILABLE  %.150s", upd_version, upd_notes);
    else if (upd_version[0]) snprintf(buf, sizeof buf, "UP TO DATE (CATALOG %s)", upd_version);
    else snprintf(buf, sizeof buf, "CATALOG UNREACHABLE");
    set_text(lbl_update, buf);
    theme::lock();
    if (upd_available) lv_obj_clear_flag(key_install, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(key_install, LV_OBJ_FLAG_HIDDEN);
    theme::unlock();
    vTaskDelete(nullptr);
}

void progress(int pct) { char b[32]; snprintf(b, sizeof b, "INSTALLING %d%%", pct); set_text(lbl_update, b); }

void install_task(void *)
{
    if (!update::install(upd_url, progress)) set_text(lbl_update, "INSTALL FAILED");
    vTaskDelete(nullptr);
}

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == 1) xTaskCreatePinnedToCore(check_task, "upd_check", 8 * 1024, nullptr, 4, nullptr, 0);
    if (k == 2 && upd_available) xTaskCreatePinnedToCore(install_task, "upd_install", 8 * 1024, nullptr, 4, nullptr, 0);
    if (k == 3 || k == 4) {
        light += k == 4 ? 10 : -10; if (light < 10) light = 10; if (light > 100) light = 100;
        bsp_display_brightness_set(light); settings::set_int("light", light);
        char b[16]; snprintf(b, sizeof b, "%d", light); lv_label_set_text(lbl_light, b);
    }
}

lv_obj_t *build()
{
    using namespace theme;
    if (scr) return scr;
    scr = screen();
    topbar::create(scr, "settings");
    const int top = PAD + topbar::HEIGHT + GAP;
    const int w = W - 2 * PAD, half = (w - GAP) / 2;

    lv_obj_t *p1 = panel(scr, PAD, top, half, 200);
    module_label(p1, "01", "NETWORK");
    lbl_ip = label(p1, ip_text, &familjen_medium_24, INK); lv_obj_align(lbl_ip, LV_ALIGN_TOP_LEFT, 0, 36);
    lbl_bridge = label(p1, bridge_text, &familjen_medium_24, MID); lv_obj_align(lbl_bridge, LV_ALIGN_TOP_LEFT, 0, 72);
    lv_obj_t *h = label(p1, "WIFI CREDENTIALS ARE COMPILED IN (main/secrets.h)", &jbmono_14, MID); lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *p2 = panel(scr, PAD + half + GAP, top, half, 200);
    module_label(p2, "02", "DISPLAY");
    lv_obj_t *cap = label(p2, "BRIGHTNESS", &jbmono_14, MID); lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 40);
    light = settings::get_int("light", 50);
    char b[16]; snprintf(b, sizeof b, "%d", light);
    lbl_light = label(p2, b, &jbmono_32, INK); lv_obj_align(lbl_light, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_t *km = keycap(p2, LV_SYMBOL_MINUS, 64, 46, false, key_cb, (void *)3); lv_obj_align(km, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(km, 0), &lv_font_montserrat_18, 0);
    lv_obj_t *kp = keycap(p2, LV_SYMBOL_PLUS, 64, 46, false, key_cb, (void *)4); lv_obj_align(kp, LV_ALIGN_BOTTOM_LEFT, 76, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(kp, 0), &lv_font_montserrat_18, 0);

    lv_obj_t *p3 = panel(scr, PAD, top + 200 + GAP, w, H - (top + 200 + GAP) - PAD);
    module_label(p3, "03", "FIRMWARE");
    char fw[80]; snprintf(fw, sizeof fw, "Tab5 Radio %s", update::running_version());
    lbl_fw = label(p3, fw, &familjen_bold_52, INK); lv_obj_align(lbl_fw, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_t *src = label(p3, "CATALOG: github.com/cloudspores/tab5", &jbmono_14, MID); lv_obj_align(src, LV_ALIGN_TOP_LEFT, 0, 100);
    lbl_update = label(p3, "TAP CHECK TO LOOK FOR A NEW VERSION", &jbmono_14, MID);
    lv_label_set_long_mode(lbl_update, LV_LABEL_LONG_WRAP); lv_obj_set_width(lbl_update, w - 36);
    lv_obj_align(lbl_update, LV_ALIGN_TOP_LEFT, 0, 128);
    lv_obj_t *kc = keycap(p3, "CHECK", 96, 46, false, key_cb, (void *)1); lv_obj_align(kc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    key_install = keycap(p3, "INSTALL UPDATE", 160, 46, true, key_cb, (void *)2); lv_obj_align(key_install, LV_ALIGN_BOTTOM_LEFT, 110, 0);
    lv_obj_add_flag(key_install, LV_OBJ_FLAG_HIDDEN);
    return scr;
}
}

namespace settings_app_ns {
void set_info(const char *ip, const char *bridge)
{
    if (ip && *ip) strlcpy(ip_text, ip, sizeof ip_text);
    if (bridge && *bridge) strlcpy(bridge_text, bridge, sizeof bridge_text);
    if (scr) { theme::lock(); lv_label_set_text(lbl_ip, ip_text); lv_label_set_text(lbl_bridge, bridge_text); theme::unlock(); }
}
}

/** On entering the screen, probe the bridge in a short-lived task so the UI never waits on it. */
static void probe_task(void *)
{
    settings_app_ns::set_info("", bridge::reachable() ? "BRIDGE: OK" : "BRIDGE: not reachable");
    vTaskDelete(nullptr);
}
static void on_enter() { xTaskCreatePinnedToCore(probe_task, "bridge_probe", 6 * 1024, nullptr, 3, nullptr, 0); }

const App settings_app = { "settings", "settings", "WiFi, bridge, display, updates", LV_SYMBOL_SETTINGS, build, on_enter, nullptr, nullptr };
