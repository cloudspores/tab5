#include "theme.h"
#include "bsp/esp-bsp.h"
#include <cmath>
#include <cstdio>

namespace theme {

void lock() { bsp_display_lock(0); }
void unlock() { bsp_display_unlock(); }

lv_obj_t *screen()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

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

void grow_on_press(lv_obj_t *obj)
{
    // Pressed state: scale to 112% about the centre, animated over 60 ms both ways. The control
    // keeps its layout slot; only its drawing (and hit area) grows while the finger is down.
    static lv_style_transition_dsc_t tr;
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, (lv_style_prop_t)0 };
    static bool init = false;
    if (!init) { lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 60, 0, nullptr); init = true; }
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(obj, 287, LV_STATE_PRESSED);   // 256 = 100 %
    lv_obj_set_style_transition(obj, &tr, LV_STATE_PRESSED);
    lv_obj_set_style_transition(obj, &tr, 0);
}

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
    lv_obj_set_ext_click_area(b, TOUCH_SLOP);            // a finger near the key still presses it
    grow_on_press(b);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
    return b;
}


// ------------------------------------------------------------------ dial
namespace {
void dial_pointer(Dial *d)
{
    int v = lv_arc_get_value(d->arc), mn = lv_arc_get_min_value(d->arc), mx = lv_arc_get_max_value(d->arc);
    float f = mx > mn ? (float)(v - mn) / (float)(mx - mn) : 0.0f;
    float ang = (135.0f + 270.0f * f) * 3.14159265f / 180.0f;     // arc angles: 0 = 3 o'clock, clockwise
    const int r_in = 12, r_out = (int)(lv_obj_get_width(d->arc) * 0.38f);
    static lv_point_precise_t pts[16][2]; static int slot = 0;
    lv_point_precise_t *p = pts[(intptr_t)d % 16];
    (void)slot;
    p[0].x = d->cx + (int)(r_in * cosf(ang));  p[0].y = d->cy + (int)(r_in * sinf(ang));
    p[1].x = d->cx + (int)(r_out * cosf(ang)); p[1].y = d->cy + (int)(r_out * sinf(ang));
    lv_line_set_points(d->ptr, p, 2);
}

void dial_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    Dial *d = (Dial *)lv_event_get_user_data(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        dial_pointer(d);
        lv_label_set_text_fmt(d->lbl, "%s %d", d->caption, (int)lv_arc_get_value(d->arc));
        if (d->on_change) d->on_change(d, (int)lv_arc_get_value(d->arc), false);
    } else if (c == LV_EVENT_RELEASED) {
        if (d->on_change) d->on_change(d, (int)lv_arc_get_value(d->arc), true);
    }
}
}

Dial *dial_create(lv_obj_t *parent, int x, int y, int size, const char *caption, int min, int max, void (*on_change)(Dial *, int, bool))
{
    Dial *d = new Dial{};
    d->caption = caption; d->on_change = on_change;
    lv_obj_t *disc = lv_obj_create(parent);
    lv_obj_set_pos(disc, x, y); lv_obj_set_size(disc, size, size);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(DISC), 0);
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
    d->cx = x + size / 2; d->cy = y + size / 2;
    d->ptr = lv_line_create(parent);
    lv_obj_set_style_line_width(d->ptr, 4, 0);
    lv_obj_set_style_line_color(d->ptr, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_line_rounded(d->ptr, true, 0);
    lv_obj_clear_flag(d->ptr, LV_OBJ_FLAG_CLICKABLE);
    d->arc = lv_arc_create(parent);
    lv_obj_set_pos(d->arc, x, y); lv_obj_set_size(d->arc, size, size);
    lv_arc_set_bg_angles(d->arc, 135, 45);
    lv_arc_set_range(d->arc, min, max);
    lv_arc_set_value(d->arc, min);
    lv_obj_set_style_arc_width(d->arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->arc, lv_color_hex(LIGHT), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(d->arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(d->arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(d->arc, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(d->arc, TOUCH_SLOP + 8);   // dials are small; catch fingers around the disc too
    d->disc = disc;
    grow_on_press(disc);                                 // the disc swells while the finger is on the arc
    lv_obj_add_event_cb(d->arc, [](lv_event_t *e) {
        Dial *dd = (Dial *)lv_event_get_user_data(e);
        lv_event_code_t c = lv_event_get_code(e);
        if (c == LV_EVENT_PRESSED) lv_obj_add_state(dd->disc, LV_STATE_PRESSED);
        else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) lv_obj_remove_state(dd->disc, LV_STATE_PRESSED);
    }, LV_EVENT_ALL, d);
    lv_obj_add_event_cb(d->arc, dial_cb, LV_EVENT_ALL, d);
    d->lbl = label(parent, caption, &jbmono_14, MID);
    lv_obj_set_width(d->lbl, size + 20);
    lv_obj_set_style_text_align(d->lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d->lbl, x - 10, y + size + 8);
    dial_pointer(d);
    return d;
}

void dial_set(Dial *d, int value)
{
    lv_arc_set_value(d->arc, value);
    dial_pointer(d);
    lv_label_set_text_fmt(d->lbl, "%s %d", d->caption, value);
}

int dial_value(Dial *d) { return (int)lv_arc_get_value(d->arc); }
void dial_caption(Dial *d, const char *text) { lv_label_set_text(d->lbl, text); }

void keycap_accent(lv_obj_t *k, bool on)
{
    lv_obj_set_style_bg_color(k, lv_color_hex(on ? ORANGE : PANEL), 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(on ? 0xc8420f : LIGHT), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(k, lv_color_hex(on ? ORANGE : LIGHT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(k, 0), lv_color_hex(on ? 0xffffff : INK), 0);
}
void keycap_text(lv_obj_t *k, const char *text) { lv_label_set_text(lv_obj_get_child(k, 0), text); }

}
