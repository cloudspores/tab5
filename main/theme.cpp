#include "theme.h"
#include "bsp/esp-bsp.h"

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

}
