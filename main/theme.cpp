#include "theme.h"
#include "bsp/esp-bsp.h"
#include <cmath>
#include <cstring>
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

void grow_on_press(lv_obj_t *obj, int percent)
{
    // Pressed state: scale to 112% about the centre, animated over 60 ms both ways. The control
    // keeps its layout slot; only its drawing (and hit area) grows while the finger is down.
    static lv_style_transition_dsc_t tr;
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, (lv_style_prop_t)0 };
    static bool init = false;
    if (!init) { lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 60, 0, nullptr); init = true; }
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(obj, 256 * percent / 100, LV_STATE_PRESSED);   // 256 = 100 %
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
    grow_on_press(b, 200);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
    return b;
}


// ------------------------------------------------------------------ dial
namespace {
// ---- lens: a 4x copy of the dial being touched, shown above (or below) the finger and turnable itself ----
void dial_pointer(Dial *d);
namespace {
struct Lens {
    lv_obj_t *box = nullptr, *disc, *hub, *ptr, *lbl, *arc;
    lv_point_precise_t pts[2];
    int size;
    Dial *current = nullptr;          ///< the small dial the lens mirrors
    lv_timer_t *closer = nullptr;     ///< closes the lens a moment after the last touch
} lens;
void lens_hide();
void lens_update(Dial *d);

/** The lens's own arc: turning it drives the small dial exactly as a finger on the small dial would. */
void lens_arc_cb(lv_event_t *e)
{
    Dial *d = lens.current;
    if (!d) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        lv_timer_pause(lens.closer);
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        int v = lv_arc_get_value(lens.arc);
        lv_arc_set_value(d->arc, v);
        dial_pointer(d);
        lv_label_set_text_fmt(d->lbl, "%s %d", d->caption, v);
        if (d->on_change) d->on_change(d, v, false);
        lens_update(d);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (d->on_change) d->on_change(d, lv_arc_get_value(lens.arc), true);
        lv_timer_reset(lens.closer); lv_timer_resume(lens.closer);
    }
}
Dial *dials[32]; int n_dials = 0;                    ///< registry (for the console's dial_press)

void lens_build()
{
    if (lens.box) return;
    lens.box = lv_obj_create(lv_screen_active());        // re-parented to whichever screen is active when shown
    lv_obj_remove_style_all(lens.box);
    lv_obj_clear_flag(lens.box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lens.disc = lv_obj_create(lens.box);
    lv_obj_set_style_radius(lens.disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lens.disc, lv_color_hex(DISC), 0);
    lv_obj_set_style_border_color(lens.disc, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_border_width(lens.disc, 2, 0);
    lv_obj_set_style_shadow_width(lens.disc, 40, 0);
    lv_obj_set_style_shadow_color(lens.disc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(lens.disc, LV_OPA_30, 0);
    lv_obj_set_style_shadow_offset_y(lens.disc, 10, 0);
    lv_obj_clear_flag(lens.disc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lens.hub = lv_obj_create(lens.box);
    lv_obj_set_style_radius(lens.hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lens.hub, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(lens.hub, 0, 0);
    lv_obj_clear_flag(lens.hub, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lens.ptr = lv_line_create(lens.box);
    lv_obj_set_style_line_color(lens.ptr, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_line_rounded(lens.ptr, true, 0);
    lv_obj_clear_flag(lens.ptr, LV_OBJ_FLAG_CLICKABLE);
    lens.lbl = label(lens.box, "", &familjen_medium_24, INK);
    lv_obj_set_style_text_align(lens.lbl, LV_TEXT_ALIGN_CENTER, 0);
    lens.arc = lv_arc_create(lens.box);                   // invisible, on top of the disc: the touch surface
    lv_arc_set_bg_angles(lens.arc, 135, 45);
    lv_obj_set_style_arc_opa(lens.arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(lens.arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(lens.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(lens.arc, 20, LV_PART_KNOB);
    lv_obj_add_event_cb(lens.arc, lens_arc_cb, LV_EVENT_ALL, nullptr);
    lens.closer = lv_timer_create([](lv_timer_t *) { lens_hide(); }, 1200, nullptr);
    lv_timer_pause(lens.closer);
    lv_obj_add_flag(lens.box, LV_OBJ_FLAG_HIDDEN);
}

void lens_update(Dial *d)
{
    if (!lens.box || lv_obj_has_flag(lens.box, LV_OBJ_FLAG_HIDDEN)) return;
    int v = lv_arc_get_value(d->arc), mn = lv_arc_get_min_value(d->arc), mx = lv_arc_get_max_value(d->arc);
    float f = mx > mn ? (float)(v - mn) / (float)(mx - mn) : 0.0f;
    float ang = (135.0f + 270.0f * f) * 3.14159265f / 180.0f;
    const int c = lens.size / 2, r_in = lens.size / 12, r_out = (int)(lens.size * 0.38f);
    lens.pts[0].x = c + (int)(r_in * cosf(ang));  lens.pts[0].y = c + (int)(r_in * sinf(ang));
    lens.pts[1].x = c + (int)(r_out * cosf(ang)); lens.pts[1].y = c + (int)(r_out * sinf(ang));
    lv_line_set_points(lens.ptr, lens.pts, 2);
    lv_label_set_text(lens.lbl, lv_label_get_text(d->lbl));
}

void lens_show(Dial *d)
{
    lens_build();
    const int size = lv_obj_get_width(d->arc) * 4;          // four times the dial
    lens.size = size;
    const int label_h = 40;
    lv_obj_set_size(lens.box, size, size + label_h);
    lv_obj_set_pos(lens.disc, 0, 0); lv_obj_set_size(lens.disc, size, size);
    lv_obj_set_size(lens.hub, size / 10, size / 10); lv_obj_set_pos(lens.hub, size / 2 - size / 20, size / 2 - size / 20);
    lv_obj_set_style_line_width(lens.ptr, size / 24, 0);
    lv_obj_set_width(lens.lbl, size); lv_obj_set_pos(lens.lbl, 0, size + 6);
    lv_obj_set_pos(lens.arc, 0, 0); lv_obj_set_size(lens.arc, size, size);
    lv_arc_set_range(lens.arc, lv_arc_get_min_value(d->arc), lv_arc_get_max_value(d->arc));
    lv_arc_set_value(lens.arc, lv_arc_get_value(d->arc));
    lens.current = d;
    lv_timer_pause(lens.closer);
    // above the dial, centred on it, kept on screen; below it when there is no room above
    lv_area_t a; lv_obj_get_coords(d->arc, &a);
    int x = (a.x1 + a.x2) / 2 - size / 2;
    const int total = size + label_h;
    int y = a.y1 - 24 - total;                            // preferred: above the dial
    if (y < 8) y = a.y2 + 24;                             // else below it
    if (y + total > H - 8) y = H - 8 - total;             // else as low as fits, overlapping the dial
    if (x < 8) x = 8;
    if (x + size > W - 8) x = W - 8 - size;
    lv_obj_set_parent(lens.box, lv_screen_active());
    lv_obj_move_foreground(lens.box);
    lv_obj_set_pos(lens.box, x, y);
    lv_obj_clear_flag(lens.box, LV_OBJ_FLAG_HIDDEN);
    lens_update(d);
}

void lens_hide()
{
    if (!lens.box) return;
    lv_obj_add_flag(lens.box, LV_OBJ_FLAG_HIDDEN);
    if (lens.current) lv_obj_remove_state(lens.current->disc, LV_STATE_PRESSED);
    lens.current = nullptr;
    lv_timer_pause(lens.closer);
}

/** Keep the lens open for a moment after the finger leaves, so it can be moved onto the big dial. */
void lens_linger() { if (lens.box && lens.current) { lv_timer_reset(lens.closer); lv_timer_resume(lens.closer); } }
} // namespace

void dial_pointer(Dial *d)
{
    int v = lv_arc_get_value(d->arc), mn = lv_arc_get_min_value(d->arc), mx = lv_arc_get_max_value(d->arc);
    float f = mx > mn ? (float)(v - mn) / (float)(mx - mn) : 0.0f;
    float ang = (135.0f + 270.0f * f) * 3.14159265f / 180.0f;     // arc angles: 0 = 3 o'clock, clockwise
    const int r_in = 12, r_out = (int)(lv_obj_get_width(d->arc) * 0.38f);
    lv_point_precise_t *p = d->pts;                       // the line keeps a pointer to these: one pair per dial
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
        if (lens.current == d) lv_arc_set_value(lens.arc, lv_arc_get_value(d->arc));
        lens_update(d);
    } else if (c == LV_EVENT_PRESSED) {
        lens_show(d);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        lens_linger();
        if (c == LV_EVENT_RELEASED && d->on_change) d->on_change(d, (int)lv_arc_get_value(d->arc), true);
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
    grow_on_press(disc, 125);                            // the disc swells a little; the lens does the real magnifying
    lv_obj_add_event_cb(d->arc, [](lv_event_t *e) {
        Dial *dd = (Dial *)lv_event_get_user_data(e);
        lv_event_code_t c = lv_event_get_code(e);
        if (c == LV_EVENT_PRESSED) lv_obj_add_state(dd->disc, LV_STATE_PRESSED);   // released by lens_hide
    }, LV_EVENT_ALL, d);
    lv_obj_add_event_cb(d->arc, dial_cb, LV_EVENT_ALL, d);
    d->lbl = label(parent, caption, &jbmono_14, MID);
    lv_obj_set_width(d->lbl, size + 20);
    lv_obj_set_style_text_align(d->lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d->lbl, x - 10, y + size + 8);
    dial_pointer(d);
    if (n_dials < 32) dials[n_dials++] = d;
    return d;
}

bool dial_press(const char *caption, bool on)
{
    for (int i = 0; i < n_dials; i++) {
        if (strcmp(dials[i]->caption, caption)) continue;
        if (on) { lv_obj_add_state(dials[i]->disc, LV_STATE_PRESSED); lens_show(dials[i]); }
        else lens_hide();
        return true;
    }
    return false;
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
