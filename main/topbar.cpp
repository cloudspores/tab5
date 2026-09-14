#include "topbar.h"
#include "theme.h"
#include <cstring>

namespace {
struct Inst { lv_obj_t *status, *dot, *output, *wifi, *clock; };
Inst inst[8]; int n = 0;
void (*home_h)() = nullptr;
char s_status[32] = "BOOT", s_out[32] = "TAB5", s_wifi[24] = "WIFI --", s_clock[8] = "--:--"; bool s_live = false;

void home_cb(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED && home_h) home_h(); }

void layout(Inst &i)
{
    lv_obj_align(i.clock, LV_ALIGN_TOP_RIGHT, -theme::PAD, theme::PAD + 3);
    lv_obj_align_to(i.wifi, i.clock, LV_ALIGN_OUT_LEFT_MID, -24, 0);
    lv_obj_align_to(i.output, i.wifi, LV_ALIGN_OUT_LEFT_MID, -24, 0);
    lv_obj_align_to(i.status, i.output, LV_ALIGN_OUT_LEFT_MID, -24, 0);
    lv_obj_align_to(i.dot, i.status, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}
void apply(Inst &i)
{
    lv_label_set_text(i.status, s_status);
    lv_obj_set_style_text_color(i.status, lv_color_hex(s_live ? theme::INK : theme::MID), 0);
    if (s_live) lv_obj_clear_flag(i.dot, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(i.dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(i.output, "OUT: %s", s_out);
    lv_label_set_text(i.wifi, s_wifi);
    lv_label_set_text(i.clock, s_clock);
    layout(i);
}
}

namespace topbar {

void create(lv_obj_t *scr, const char *app_name)
{
    using namespace theme;
    lv_obj_t *name = label(scr, app_name, &familjen_semibold_18, INK);
    lv_obj_set_pos(name, PAD, PAD);
    lv_obj_add_flag(name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(name, 16);
    lv_obj_add_event_cb(name, home_cb, LV_EVENT_ALL, nullptr);
    lv_obj_t *sub = label(scr, "TAB5 . ESP32-P4", &jbmono_14, MID);
    lv_obj_align_to(sub, name, LV_ALIGN_OUT_RIGHT_BOTTOM, 18, -1);
    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_set_pos(rule, PAD, PAD + HEIGHT); lv_obj_set_size(rule, W - 2 * PAD, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(rule, 0, 0); lv_obj_set_style_radius(rule, 0, 0);
    if (n >= 8) return;
    Inst &i = inst[n++];
    i.clock = label(scr, "--:--", &jbmono_14, INK);
    i.wifi = label(scr, "", &jbmono_14, MID);
    i.output = label(scr, "", &jbmono_14, MID);
    i.status = label(scr, "", &jbmono_14, MID);
    i.dot = lv_obj_create(scr);
    lv_obj_set_size(i.dot, 10, 10);
    lv_obj_set_style_radius(i.dot, 5, 0);
    lv_obj_set_style_border_width(i.dot, 0, 0);
    lv_obj_set_style_bg_color(i.dot, lv_color_hex(ORANGE), 0);
    apply(i);
}

void on_home(void (*h)()) { home_h = h; }
void set_status(const char *t, bool live) { strlcpy(s_status, t, sizeof s_status); s_live = live; theme::lock(); for (int k = 0; k < n; k++) apply(inst[k]); theme::unlock(); }
void set_output(const char *t) { strlcpy(s_out, t, sizeof s_out); theme::lock(); for (int k = 0; k < n; k++) apply(inst[k]); theme::unlock(); }
void set_wifi(const char *t)   { strlcpy(s_wifi, t, sizeof s_wifi); theme::lock(); for (int k = 0; k < n; k++) apply(inst[k]); theme::unlock(); }
void set_clock(const char *t)  { strlcpy(s_clock, t, sizeof s_clock); theme::lock(); for (int k = 0; k < n; k++) apply(inst[k]); theme::unlock(); }

}
