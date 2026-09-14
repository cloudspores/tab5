#include "ui.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include "bsp/esp-bsp.h"
#include "lvgl.h"

LV_FONT_DECLARE(familjen_bold_52);
LV_FONT_DECLARE(familjen_medium_24);
LV_FONT_DECLARE(familjen_semibold_18);
LV_FONT_DECLARE(familjen_medium_14);
LV_FONT_DECLARE(jbmono_14);
LV_FONT_DECLARE(jbmono_32);

namespace {

constexpr uint32_t BG = 0xd4d1c9, PANEL = 0xebeae4, INK = 0x1c1c1a, MID = 0x76746e, LIGHT = 0xb5b2aa, ORANGE = 0xff5a1f;
constexpr int W = 1280, H = 720, PAD = 24, GAP = 16;
constexpr int METER_BARS = 48, METER_ROWS = 8;
constexpr int PRESETS = 6;

lv_obj_t *lbl_status, *lbl_clock, *lbl_output, *dot_onair;
lv_obj_t *lbl_station, *lbl_title, *lbl_format;
lv_obj_t *dial_arc[2], *dial_ptr[2], *dial_lbl[2]; int dial_cx[2], dial_cy[2];
lv_obj_t *key_play_lbl, *key_mute_lbl;
int brightness_pct = 50;
lv_obj_t *band, *band_items[64]; int band_count = 0, band_current = -1;
lv_obj_t *preset_keys[PRESETS], *preset_names[PRESETS];
lv_obj_t *meter_dots[METER_BARS][METER_ROWS];
uint8_t   meter_state[METER_BARS][METER_ROWS];
uint8_t   levels[METER_BARS]; int level_head = 0;
lv_obj_t *overlay, *ta, *kb, *results;

ui::KeyHandler key_h; ui::IndexHandler tune_h; ui::PresetHandler preset_h; ui::TextHandler search_h; ui::IndexHandler result_h; ui::DialHandler dial_h;

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y); lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_pad_all(p, 18, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void module_label(lv_obj_t *parent, const char *num, const char *name)
{
    lv_obj_t *n = label(parent, num, &jbmono_14, INK);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *t = label(parent, name, &jbmono_14, MID);
    lv_obj_align_to(t, n, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
}

// Keycap: square button with mono text, optional caption below.
lv_obj_t *keycap(lv_obj_t *parent, const char *text, int w, int h, bool accent, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent ? ORANGE : PANEL), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent ? 0xc8420f : LIGHT), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(accent ? ORANGE : LIGHT), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = label(b, text, &jbmono_14, accent ? 0xffffff : INK);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
    return b;
}

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (key_h) key_h((ui::Key)(intptr_t)lv_event_get_user_data(e));
}

void band_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (tune_h) tune_h((int)(intptr_t)lv_event_get_user_data(e));
}

void preset_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    static bool long_pressed[PRESETS];
    if (c == LV_EVENT_LONG_PRESSED) { long_pressed[slot] = true; if (preset_h) preset_h(slot, true); }
    else if (c == LV_EVENT_PRESSED) long_pressed[slot] = false;
    else if (c == LV_EVENT_CLICKED && !long_pressed[slot]) { if (preset_h) preset_h(slot, false); }
}

void result_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (result_h) result_h((int)(intptr_t)lv_event_get_user_data(e));
}

void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) { if (search_h) search_h(lv_textarea_get_text(ta)); }
    else if (c == LV_EVENT_CANCEL) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void close_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

// Pointer line from near the centre to the rim, at the arc's current angle.
void dial_update_pointer(int d)
{
    lv_obj_t *arc = dial_arc[d];
    int v = lv_arc_get_value(arc), mn = lv_arc_get_min_value(arc), mx = lv_arc_get_max_value(arc);
    float f = mx > mn ? (float)(v - mn) / (float)(mx - mn) : 0.0f;
    float ang = (135.0f + 270.0f * f) * 3.14159265f / 180.0f;    // LVGL arc angles: 0 = 3 o'clock, clockwise
    static lv_point_precise_t pts[2][2];
    const int r_in = 12, r_out = 40;
    pts[d][0].x = dial_cx[d] + (int)(r_in * cosf(ang));  pts[d][0].y = dial_cy[d] + (int)(r_in * sinf(ang));
    pts[d][1].x = dial_cx[d] + (int)(r_out * cosf(ang)); pts[d][1].y = dial_cy[d] + (int)(r_out * sinf(ang));
    lv_line_set_points(dial_ptr[d], pts[d], 2);
}

void dial_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        dial_update_pointer(d);
        if (d == ui::DIAL_VOL && dial_h) dial_h(d, lv_arc_get_value(dial_arc[d]));
        if (d == ui::DIAL_TUNE) lv_label_set_text_fmt(dial_lbl[d], "TUNE %02d", (int)lv_arc_get_value(dial_arc[d]) + 1);
        if (d == ui::DIAL_VOL) lv_label_set_text_fmt(dial_lbl[d], "VOL %d", (int)lv_arc_get_value(dial_arc[d]));
    } else if (c == LV_EVENT_RELEASED && d == ui::DIAL_TUNE && dial_h) {
        dial_h(d, lv_arc_get_value(dial_arc[d]));
    }
}

// A rotary dial: disc, touch arc (knob hidden), orange pointer, caption below.
void make_dial(lv_obj_t *parent, int d, int x, int y, const char *caption, int min, int max)
{
    const int size = 104;
    lv_obj_t *disc = lv_obj_create(parent);
    lv_obj_set_pos(disc, x, y); lv_obj_set_size(disc, size, size);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0xf6f5f1), 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_border_width(disc, 1, 0);
    lv_obj_set_style_shadow_width(disc, 12, 0);
    lv_obj_set_style_shadow_color(disc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(disc, LV_OPA_10, 0);
    lv_obj_set_style_shadow_offset_y(disc, 3, 0);
    lv_obj_clear_flag(disc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *hub = lv_obj_create(parent);
    lv_obj_set_size(hub, 8, 8);
    lv_obj_set_pos(hub, x + size / 2 - 4, y + size / 2 - 4);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_clear_flag(hub, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    dial_cx[d] = x + size / 2; dial_cy[d] = y + size / 2;
    dial_ptr[d] = lv_line_create(parent);
    lv_obj_set_style_line_width(dial_ptr[d], 4, 0);
    lv_obj_set_style_line_color(dial_ptr[d], lv_color_hex(ORANGE), 0);
    lv_obj_set_style_line_rounded(dial_ptr[d], true, 0);
    lv_obj_clear_flag(dial_ptr[d], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_pos(arc, x, y); lv_obj_set_size(arc, size, size);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_range(arc, min, max);
    lv_arc_set_value(arc, min);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(LIGHT), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(arc, dial_cb, LV_EVENT_ALL, (void *)(intptr_t)d);
    dial_arc[d] = arc;
    dial_lbl[d] = label(parent, caption, &jbmono_14, MID);
    lv_obj_set_width(dial_lbl[d], size);
    lv_obj_set_style_text_align(dial_lbl[d], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(dial_lbl[d], x, y + size + 8);
    dial_update_pointer(d);
}

// Icon key: LVGL symbol glyph (FontAwesome subset in the Montserrat font) on a keycap.
lv_obj_t *icon_key(lv_obj_t *parent, const char *symbol, bool accent, lv_event_cb_t cb, void *ud, lv_obj_t **out_lbl)
{
    lv_obj_t *b = keycap(parent, "", 64, 46, accent, cb, ud);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

// Brightness key: a small drawn sun (disc + eight rays), no glyph needed.
lv_obj_t *sun_key(lv_obj_t *parent, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = keycap(parent, "", 64, 46, false, cb, ud);
    lv_obj_t *core = lv_obj_create(b);
    lv_obj_set_size(core, 10, 10);
    lv_obj_center(core);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(core, 0, 0);
    lv_obj_clear_flag(core, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    static lv_point_precise_t rays[8][2];
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4.0f;
        rays[i][0].x = 32 + (int)(8 * cosf(a));  rays[i][0].y = 23 + (int)(8 * sinf(a));
        rays[i][1].x = 32 + (int)(12 * cosf(a)); rays[i][1].y = 23 + (int)(12 * sinf(a));
        lv_obj_t *r = lv_line_create(b);
        lv_line_set_points(r, rays[i], 2);
        lv_obj_set_style_line_width(r, 2, 0);
        lv_obj_set_style_line_color(r, lv_color_hex(INK), 0);
        lv_obj_set_style_line_rounded(r, true, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    return b;
}

void power_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED && key_h) key_h(ui::KEY_POWER);
}

void meter_redraw(lv_timer_t *)
{
    for (int c = 0; c < METER_BARS; c++) {
        int lit = (levels[(level_head + c) % METER_BARS] * METER_ROWS + 50) / 100;
        for (int r = 0; r < METER_ROWS; r++) {
            bool on = (METER_ROWS - r) <= lit;
            uint8_t st = on ? (r <= 1 ? 2 : 1) : 0;
            if (st == meter_state[c][r]) continue;
            meter_state[c][r] = st;
            lv_obj_set_style_bg_color(meter_dots[c][r], lv_color_hex(st == 2 ? ORANGE : st == 1 ? INK : LIGHT), 0);
        }
    }
}

void style_band_item(int i, bool current)
{
    lv_obj_t *b = band_items[i];
    lv_obj_set_style_bg_color(b, lv_color_hex(current ? ORANGE : PANEL), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(current ? ORANGE : LIGHT), 0);
    lv_obj_t *num = lv_obj_get_child(b, 0), *name = lv_obj_get_child(b, 1);
    lv_obj_set_style_text_color(num, lv_color_hex(current ? 0xffffff : MID), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(current ? 0xffffff : INK), 0);
}

} // namespace

namespace ui {

void init()
{
    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- top rule ----
    lv_obj_t *top_l = label(scr, "internet radio", &familjen_semibold_18, INK);
    lv_obj_set_pos(top_l, PAD, PAD);
    lv_obj_t *top_sub = label(scr, "TAB5 . ESP32-P4", &jbmono_14, MID);
    lv_obj_align_to(top_sub, top_l, LV_ALIGN_OUT_RIGHT_BOTTOM, 18, -1);
    lbl_clock = label(scr, "--:--", &jbmono_14, INK);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -PAD, PAD + 3);
    lbl_output = label(scr, "OUT: TAB5", &jbmono_14, MID);
    lv_obj_align_to(lbl_output, lbl_clock, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    lv_obj_add_flag(lbl_output, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_output, 16);
    lv_obj_add_event_cb(lbl_output, key_cb, LV_EVENT_ALL, (void *)KEY_OUTPUT);
    lbl_status = label(scr, "BOOT", &jbmono_14, MID);
    lv_obj_align_to(lbl_status, lbl_output, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    dot_onair = lv_obj_create(scr);
    lv_obj_set_size(dot_onair, 10, 10);
    lv_obj_set_style_radius(dot_onair, 5, 0);
    lv_obj_set_style_border_width(dot_onair, 0, 0);
    lv_obj_set_style_bg_color(dot_onair, lv_color_hex(ORANGE), 0);
    lv_obj_add_flag(dot_onair, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_set_pos(rule, PAD, PAD + 32); lv_obj_set_size(rule, W - 2 * PAD, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(rule, 0, 0); lv_obj_set_style_radius(rule, 0, 0);

    // ---- row 1: 01 STATION + 02 CONTROL ----
    const int row1_y = PAD + 32 + GAP, row1_h = 280, ctrl_w = 340;
    const int st_w = W - 2 * PAD - ctrl_w - GAP;
    lv_obj_t *p_station = panel(scr, PAD, row1_y, st_w, row1_h);
    module_label(p_station, "01", "STATION");
    lbl_station = label(p_station, "--", &familjen_bold_52, INK);
    lv_label_set_long_mode(lbl_station, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_station, st_w - 36);
    lv_obj_align(lbl_station, LV_ALIGN_LEFT_MID, 0, -22);
    lbl_title = label(p_station, "", &familjen_medium_24, MID);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_title, st_w - 36);
    lv_obj_align_to(lbl_title, lbl_station, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
    lbl_format = label(p_station, "", &jbmono_14, MID);
    lv_obj_align(lbl_format, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(p_station, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p_station, key_cb, LV_EVENT_ALL, (void *)KEY_NEXT);

    lv_obj_t *p_ctrl = panel(scr, W - PAD - ctrl_w, row1_y, ctrl_w, row1_h);
    module_label(p_ctrl, "02", "CONTROL");
    make_dial(p_ctrl, DIAL_TUNE, 12, 30, "TUNE --", 0, 1);
    make_dial(p_ctrl, DIAL_VOL, ctrl_w - 36 - 104 - 12, 30, "VOL 60", 0, 100);
    lv_obj_t *row = lv_obj_create(p_ctrl);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ctrl_w - 36, 46);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon_key(row, LV_SYMBOL_PAUSE, true, key_cb, (void *)KEY_PLAY, &key_play_lbl);
    icon_key(row, LV_SYMBOL_VOLUME_MAX, false, key_cb, (void *)KEY_MUTE, &key_mute_lbl);
    sun_key(row, key_cb, (void *)KEY_LIGHT);
    lv_obj_t *pw = icon_key(row, LV_SYMBOL_POWER, false, nullptr, nullptr, nullptr);
    lv_obj_add_event_cb(pw, power_cb, LV_EVENT_ALL, nullptr);

    // ---- row 2: 03 SIGNAL ----
    const int row2_y = row1_y + row1_h + GAP, row2_h = 140;
    lv_obj_t *p_sig = panel(scr, PAD, row2_y, W - 2 * PAD, row2_h);
    module_label(p_sig, "03", "SIGNAL");
    const int dot = 10, dgap = 4, inner_w = W - 2 * PAD - 36, step = (inner_w - dot) / (METER_BARS - 1);
    for (int c = 0; c < METER_BARS; c++)
        for (int r = 0; r < METER_ROWS; r++) {
            lv_obj_t *d = lv_obj_create(p_sig);
            lv_obj_set_size(d, dot, dot);
            lv_obj_set_pos(d, c * step, 26 + r * (dot + dgap));
            lv_obj_set_style_bg_color(d, lv_color_hex(LIGHT), 0);
            lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_radius(d, 2, 0);
            lv_obj_set_style_pad_all(d, 0, 0);
            lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
            meter_dots[c][r] = d;
        }
    lv_timer_create(meter_redraw, 66, nullptr);

    // ---- row 3: 04 TUNE + 05 PRESETS ----
    const int row3_y = row2_y + row2_h + GAP, row3_h = H - row3_y - PAD;
    const int pre_w = 470, tune_w = W - 2 * PAD - pre_w - GAP;
    lv_obj_t *p_tune = panel(scr, PAD, row3_y, tune_w, row3_h);
    module_label(p_tune, "04", "TUNE");
    lv_obj_t *keys = lv_obj_create(p_tune);
    lv_obj_remove_style_all(keys);
    lv_obj_set_size(keys, 96, row3_h - 36 - 26);
    lv_obj_align(keys, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    keycap(keys, "SEARCH", 96, 34, true, key_cb, (void *)KEY_SEARCH);
    keycap(keys, "CR TOP", 96, 34, false, key_cb, (void *)KEY_CR);
    keycap(keys, "HOME", 96, 34, false, key_cb, (void *)KEY_HOME);
    band = lv_obj_create(p_tune);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, tune_w - 36 - 96 - 14, row3_h - 36 - 26);
    lv_obj_align(band, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(band, 10, 0);
    lv_obj_set_scroll_dir(band, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(band, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *p_pre = panel(scr, W - PAD - pre_w, row3_y, pre_w, row3_h);
    module_label(p_pre, "05", "PRESETS");
    lv_obj_t *hold = label(p_pre, "HOLD TO STORE", &jbmono_14, MID);
    lv_obj_align(hold, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *prow = lv_obj_create(p_pre);
    lv_obj_remove_style_all(prow);
    lv_obj_set_size(prow, pre_w - 36, row3_h - 36 - 26);
    lv_obj_align(prow, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < PRESETS; i++) {
        lv_obj_t *col = lv_obj_create(prow);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 64, row3_h - 36 - 26);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 6, 0);
        char n[4]; snprintf(n, sizeof n, "%d", i + 1);
        preset_keys[i] = keycap(col, n, 56, 56, false, preset_cb, (void *)(intptr_t)i);
        preset_names[i] = label(col, "", &familjen_medium_14, MID);
        lv_label_set_long_mode(preset_names[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(preset_names[i], 64);
        lv_obj_set_style_text_align(preset_names[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // ---- search overlay ----
    overlay = lv_obj_create(scr);
    lv_obj_set_pos(overlay, 0, 0); lv_obj_set_size(overlay, W, H);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(BG), 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, PAD, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ol = label(overlay, "SEARCH STATIONS", &jbmono_14, MID);
    lv_obj_align(ol, LV_ALIGN_TOP_LEFT, 0, 4);
    keycap(overlay, "CLOSE", 96, 34, false, close_cb, nullptr);
    lv_obj_align(lv_obj_get_child(overlay, -1), LV_ALIGN_TOP_RIGHT, 0, 0);
    ta = lv_textarea_create(overlay);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "station name, genre, city");
    lv_obj_set_size(ta, W - 2 * PAD, 52);
    lv_obj_set_pos(ta, 0, 40);
    lv_obj_set_style_text_font(ta, &familjen_medium_24, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_radius(ta, 6, 0);
    results = lv_list_create(overlay);
    lv_obj_set_size(results, W - 2 * PAD, 260);
    lv_obj_set_pos(results, 0, 104);
    lv_obj_set_style_bg_color(results, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_color(results, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_radius(results, 6, 0);
    kb = lv_keyboard_create(overlay);
    lv_obj_set_size(kb, W - 2 * PAD, 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_ALL, nullptr);

    bsp_display_unlock();
}

void on_key(KeyHandler h)       { key_h = h; }
void on_tune(IndexHandler h)    { tune_h = h; }
void on_preset(PresetHandler h) { preset_h = h; }
void on_search(TextHandler h)   { search_h = h; }
void on_result(IndexHandler h)  { result_h = h; }
void on_dial(DialHandler h)     { dial_h = h; }

static void realign_top()
{
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -PAD, PAD + 3);
    lv_obj_align_to(lbl_output, lbl_clock, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    lv_obj_align_to(lbl_status, lbl_output, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    lv_obj_align_to(dot_onair, lbl_status, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}

void set_status(const char *text)
{
    bsp_display_lock(0);
    bool on = strcmp(text, "ON AIR") == 0;
    lv_label_set_text(lbl_status, text);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(on ? INK : MID), 0);
    if (on) lv_obj_clear_flag(dot_onair, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(dot_onair, LV_OBJ_FLAG_HIDDEN);
    realign_top();
    bsp_display_unlock();
}

void set_clock(const char *text)  { bsp_display_lock(0); lv_label_set_text(lbl_clock, text); realign_top(); bsp_display_unlock(); }
void set_output(const char *name) { bsp_display_lock(0); lv_label_set_text_fmt(lbl_output, "OUT: %s", name); realign_top(); bsp_display_unlock(); }
void set_station(const char *name) { bsp_display_lock(0); lv_label_set_text(lbl_station, name); bsp_display_unlock(); }
void set_title(const char *text)   { bsp_display_lock(0); lv_label_set_text(lbl_title, text); bsp_display_unlock(); }

void set_format(const char *codec, int kbps, int sample_rate, int channels)
{
    char buf[64];
    if (kbps > 0) snprintf(buf, sizeof buf, "%s %dK . %d.%d KHZ . %s", codec, kbps, sample_rate / 1000, (sample_rate % 1000) / 100, channels == 1 ? "MONO" : "STEREO");
    else          snprintf(buf, sizeof buf, "%s . %d.%d KHZ . %s", codec, sample_rate / 1000, (sample_rate % 1000) / 100, channels == 1 ? "MONO" : "STEREO");
    bsp_display_lock(0); lv_label_set_text(lbl_format, buf); bsp_display_unlock();
}

void set_volume(int percent, bool muted)
{
    bsp_display_lock(0);
    lv_arc_set_value(dial_arc[DIAL_VOL], percent);
    dial_update_pointer(DIAL_VOL);
    if (muted) lv_label_set_text(dial_lbl[DIAL_VOL], "MUTED"); else lv_label_set_text_fmt(dial_lbl[DIAL_VOL], "VOL %d", percent);
    lv_obj_set_style_text_color(dial_lbl[DIAL_VOL], lv_color_hex(muted ? ORANGE : MID), 0);
    lv_label_set_text(key_mute_lbl, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    bsp_display_unlock();
}

void set_playing(bool playing)
{
    bsp_display_lock(0);
    lv_label_set_text(key_play_lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    bsp_display_unlock();
}

void set_brightness_text(int percent) { brightness_pct = percent; }

void set_band(const std::vector<std::string> &names, int current)
{
    bsp_display_lock(0);
    lv_obj_clean(band);
    band_count = 0;
    for (size_t i = 0; i < names.size() && i < 64; i++) {
        lv_obj_t *b = lv_button_create(band);
        lv_obj_set_size(b, 150, LV_PCT(100));
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 10, 0);
        char num[4]; snprintf(num, sizeof num, "%02d", (int)i + 1);
        lv_obj_t *n = label(b, num, &jbmono_14, MID);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *nm = label(b, names[i].c_str(), &familjen_medium_14, INK);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(nm, 130);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(b, band_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        band_items[band_count++] = b;
        style_band_item(i, (int)i == current);
    }
    band_current = current;
    if (current >= 0 && current < band_count) lv_obj_scroll_to_view(band_items[current], LV_ANIM_OFF);
    lv_arc_set_range(dial_arc[DIAL_TUNE], 0, band_count > 1 ? band_count - 1 : 1);
    lv_arc_set_value(dial_arc[DIAL_TUNE], current < 0 ? 0 : current);
    dial_update_pointer(DIAL_TUNE);
    if (current >= 0) lv_label_set_text_fmt(dial_lbl[DIAL_TUNE], "TUNE %02d", current + 1); else lv_label_set_text(dial_lbl[DIAL_TUNE], "TUNE --");
    bsp_display_unlock();
}

void set_band_current(int current)
{
    bsp_display_lock(0);
    if (band_current >= 0 && band_current < band_count) style_band_item(band_current, false);
    band_current = current;
    if (current >= 0 && current < band_count) { style_band_item(current, true); lv_obj_scroll_to_view(band_items[current], LV_ANIM_ON); }
    if (current >= 0) { lv_arc_set_value(dial_arc[DIAL_TUNE], current); dial_update_pointer(DIAL_TUNE); lv_label_set_text_fmt(dial_lbl[DIAL_TUNE], "TUNE %02d", current + 1); }
    bsp_display_unlock();
}

void set_presets(const std::vector<std::string> &names)
{
    bsp_display_lock(0);
    for (int i = 0; i < PRESETS; i++) lv_label_set_text(preset_names[i], i < (int)names.size() ? names[i].c_str() : "");
    bsp_display_unlock();
}

void show_results(const std::vector<std::string> &names)
{
    bsp_display_lock(0);
    lv_obj_clean(results);
    if (names.empty()) lv_list_add_text(results, "no stations found");
    for (size_t i = 0; i < names.size(); i++) {
        lv_obj_t *b = lv_list_add_button(results, nullptr, names[i].c_str());
        lv_obj_set_style_text_font(b, &familjen_semibold_18, 0);
        lv_obj_add_event_cb(b, result_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
    }
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void open_search() { bsp_display_lock(0); lv_obj_clean(results); lv_textarea_set_text(ta, ""); lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN); bsp_display_unlock(); }
void close_search() { bsp_display_lock(0); lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN); bsp_display_unlock(); }

void push_level(uint8_t level) { levels[level_head] = level; level_head = (level_head + 1) % METER_BARS; }

} // namespace ui
