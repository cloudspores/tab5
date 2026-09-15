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


// ---- dials and the lens --------------------------------------------------------------------
// A dial is a light disc with an orange pointer over an invisible touch surface. The value
// follows the finger's angle around the disc centre (135 deg = min, 405 deg = max, the dead
// zone at the bottom snaps to the nearer end), computed here rather than by lv_arc so any
// finger position inside the disc works. The lens is a 4x copy of the dial being touched,
// with its own touch surface driving the same dial. Drawing is flat: soft shadows cost the
// software renderer more per pointer move than the whole rest of the screen.
void dial_pointer(Dial *d);
namespace {

struct Lens {
    lv_obj_t *box = nullptr, *disc, *hub, *ptr, *lbl, *touch;
    lv_point_precise_t pts[2];
    int size;
    Dial *current = nullptr;          ///< the small dial the lens mirrors
    lv_timer_t *closer = nullptr;     ///< closes the lens a moment after the last touch
} lens;
Dial *dials[32]; int n_dials = 0;     ///< registry (for the console's dial_press)

void lens_update(Dial *d);
void lens_hide();

/** Pointer angle for a value: 135 deg at min, clockwise 270 deg to max (screen y points down). */
float dial_angle(const Dial *d)
{
    float f = d->max > d->min ? (float)(d->value - d->min) / (float)(d->max - d->min) : 0.0f;
    return (135.0f + 270.0f * f) * 3.14159265f / 180.0f;
}

/** Value for a finger at (px,py) relative to the disc centre. */
int value_from_touch(const Dial *d, int px, int py)
{
    float deg = atan2f((float)py, (float)px) * 180.0f / 3.14159265f;   // clockwise from 3 o'clock
    float a = deg - 135.0f;
    while (a < 0) a += 360.0f;
    if (a > 270.0f) a = a < 315.0f ? 270.0f : 0.0f;                     // dead zone at the bottom
    return d->min + (int)((d->max - d->min) * a / 270.0f + 0.5f);
}

/** Apply a new value from a finger: pointer, caption, lens and the owner's callback. */
void dial_apply(Dial *d, int v, bool released)
{
    if (v < d->min) v = d->min; else if (v > d->max) v = d->max;
    if (v != d->value) {
        d->value = v;
        dial_pointer(d);
        lv_label_set_text_fmt(d->lbl, "%s %d", d->caption, v);
        lens_update(d);
        if (d->on_change) d->on_change(d, v, false);
    }
    if (released && d->on_change) d->on_change(d, v, true);
}

/** Shared touch handling for the small dial and the lens: `cx,cy` is the surface centre. */
void track_touch(Dial *d, lv_event_t *e, lv_obj_t *surface)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    lv_area_t a; lv_obj_get_coords(surface, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    int dx = p.x - cx, dy = p.y - cy;
    if (dx * dx + dy * dy < 36) return;                                 // too close to the hub: no angle
    dial_apply(d, value_from_touch(d, dx, dy), false);
}

void lens_touch_cb(lv_event_t *e)
{
    Dial *d = lens.current;
    if (!d) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) lv_timer_pause(lens.closer);
    if (c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING) track_touch(d, e, lens.touch);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        dial_apply(d, d->value, true);
        lv_timer_reset(lens.closer); lv_timer_resume(lens.closer);
    }
}

lv_obj_t *flat_circle(lv_obj_t *parent, uint32_t colour, int border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

lv_obj_t *touch_surface(lv_obj_t *parent, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_add_flag(t, LV_OBJ_FLAG_PRESS_LOCK);                         // keep tracking when the finger leaves the disc
    lv_obj_add_event_cb(t, cb, LV_EVENT_ALL, ud);
    return t;
}

void lens_build()
{
    if (lens.box) return;
    lens.box = lv_obj_create(lv_screen_active());        // re-parented to whichever screen is active when shown
    lv_obj_remove_style_all(lens.box);
    lv_obj_clear_flag(lens.box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lens.disc = flat_circle(lens.box, DISC, 2);
    lens.hub = flat_circle(lens.box, INK, 0);
    lens.ptr = lv_line_create(lens.box);
    lv_obj_set_style_line_color(lens.ptr, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_line_rounded(lens.ptr, true, 0);
    lv_obj_clear_flag(lens.ptr, LV_OBJ_FLAG_CLICKABLE);
    lens.lbl = label(lens.box, "", &familjen_medium_24, INK);
    lv_obj_set_style_text_align(lens.lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(lens.lbl, lv_color_hex(DISC), 0);       // a plate, so the reading never overprints
    lv_obj_set_style_bg_opa(lens.lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lens.lbl, 8, 0);
    lv_obj_set_style_pad_ver(lens.lbl, 6, 0);
    lens.touch = touch_surface(lens.box, lens_touch_cb, nullptr);
    lens.closer = lv_timer_create([](lv_timer_t *) { lens_hide(); }, 1200, nullptr);
    lv_timer_pause(lens.closer);
    lv_obj_add_flag(lens.box, LV_OBJ_FLAG_HIDDEN);
}

void lens_update(Dial *d)
{
    if (!lens.box || lens.current != d || lv_obj_has_flag(lens.box, LV_OBJ_FLAG_HIDDEN)) return;
    float ang = dial_angle(d);
    const int c = lens.size / 2, r_in = lens.size / 12, r_out = (int)(lens.size * 0.38f);
    lens.pts[0].x = c + (int)(r_in * cosf(ang));  lens.pts[0].y = c + (int)(r_in * sinf(ang));
    lens.pts[1].x = c + (int)(r_out * cosf(ang)); lens.pts[1].y = c + (int)(r_out * sinf(ang));
    lv_line_set_points(lens.ptr, lens.pts, 2);
    lv_label_set_text(lens.lbl, lv_label_get_text(d->lbl));
}

void lens_show(Dial *d)
{
    lens_build();
    const int size = lv_obj_get_width(d->touch) * 4;        // four times the dial
    lens.size = size;
    const int label_h = 52;
    lv_obj_set_size(lens.box, size, size + label_h);
    lv_obj_set_pos(lens.disc, 0, 0); lv_obj_set_size(lens.disc, size, size);
    lv_obj_set_size(lens.hub, size / 10, size / 10); lv_obj_set_pos(lens.hub, size / 2 - size / 20, size / 2 - size / 20);
    lv_obj_set_style_line_width(lens.ptr, size / 24, 0);
    lv_obj_set_width(lens.lbl, size); lv_obj_set_pos(lens.lbl, 0, size + 6);
    lv_obj_set_pos(lens.touch, 0, 0); lv_obj_set_size(lens.touch, size, size);
    lv_obj_move_foreground(lens.touch);
    lens.current = d;
    lv_timer_pause(lens.closer);
    // above the dial, centred on it, kept on screen; below it when there is no room above
    lv_area_t a; lv_obj_get_coords(d->touch, &a);
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
    lens.current = nullptr;
    lv_timer_pause(lens.closer);
}

/** Keep the lens open for a moment after the finger leaves, so it can be moved onto the big dial. */
void lens_linger() { if (lens.box && lens.current) { lv_timer_reset(lens.closer); lv_timer_resume(lens.closer); } }

void dial_touch_cb(lv_event_t *e)
{
    Dial *d = (Dial *)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) { lens_show(d); track_touch(d, e, d->touch); }
    else if (c == LV_EVENT_PRESSING) track_touch(d, e, d->touch);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) { dial_apply(d, d->value, true); lens_linger(); }
}
} // namespace

void dial_pointer(Dial *d)
{
    float ang = dial_angle(d);
    const int r_in = 12, r_out = (int)(lv_obj_get_width(d->touch) * 0.38f);
    lv_point_precise_t *p = d->pts;                       // the line keeps a pointer to these: one pair per dial
    p[0].x = d->cx + (int)(r_in * cosf(ang));  p[0].y = d->cy + (int)(r_in * sinf(ang));
    p[1].x = d->cx + (int)(r_out * cosf(ang)); p[1].y = d->cy + (int)(r_out * sinf(ang));
    lv_line_set_points(d->ptr, p, 2);
}

Dial *dial_create(lv_obj_t *parent, int x, int y, int size, const char *caption, int min, int max, void (*on_change)(Dial *, int, bool))
{
    Dial *d = new Dial{};
    d->caption = caption; d->on_change = on_change; d->min = min; d->max = max; d->value = min;
    d->disc = flat_circle(parent, DISC, 1);
    lv_obj_set_pos(d->disc, x, y); lv_obj_set_size(d->disc, size, size);
    lv_obj_t *hub = flat_circle(parent, INK, 0);
    lv_obj_set_size(hub, 8, 8);
    lv_obj_set_pos(hub, x + size / 2 - 4, y + size / 2 - 4);
    d->cx = x + size / 2; d->cy = y + size / 2;
    d->ptr = lv_line_create(parent);
    lv_obj_set_style_line_width(d->ptr, 4, 0);
    lv_obj_set_style_line_color(d->ptr, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_line_rounded(d->ptr, true, 0);
    lv_obj_clear_flag(d->ptr, LV_OBJ_FLAG_CLICKABLE);
    d->touch = touch_surface(parent, dial_touch_cb, d);
    lv_obj_set_pos(d->touch, x, y); lv_obj_set_size(d->touch, size, size);
    lv_obj_set_ext_click_area(d->touch, TOUCH_SLOP + 8);   // dials are small; catch fingers around the disc too
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
        if (on) lens_show(dials[i]); else lens_hide();
        return true;
    }
    return false;
}

void dial_set(Dial *d, int value)
{
    d->value = value < d->min ? d->min : value > d->max ? d->max : value;
    dial_pointer(d);
    lv_label_set_text_fmt(d->lbl, "%s %d", d->caption, d->value);
    lens_update(d);
}

int dial_value(Dial *d) { return d->value; }
void dial_caption(Dial *d, const char *text) { lv_label_set_text(d->lbl, text); if (lens.current == d) lens_update(d); }

void keycap_accent(lv_obj_t *k, bool on)
{
    lv_obj_set_style_bg_color(k, lv_color_hex(on ? ORANGE : PANEL), 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(on ? 0xc8420f : LIGHT), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(k, lv_color_hex(on ? ORANGE : LIGHT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(k, 0), lv_color_hex(on ? 0xffffff : INK), 0);
}
void keycap_text(lv_obj_t *k, const char *text) { lv_label_set_text(lv_obj_get_child(k, 0), text); }

}
