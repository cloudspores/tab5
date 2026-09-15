// Instrument-panel theme shared by every app: palette, fonts, and the panel/label/keycap primitives.
#pragma once
#include "lvgl.h"

LV_FONT_DECLARE(familjen_bold_52);
LV_FONT_DECLARE(familjen_medium_24);
LV_FONT_DECLARE(familjen_semibold_18);
LV_FONT_DECLARE(familjen_medium_14);
LV_FONT_DECLARE(jbmono_14);
LV_FONT_DECLARE(jbmono_32);

namespace theme {
constexpr uint32_t BG = 0xd4d1c9, PANEL = 0xebeae4, INK = 0x1c1c1a, MID = 0x76746e, LIGHT = 0xb5b2aa, ORANGE = 0xff5a1f, DISC = 0xf6f5f1;
constexpr int W = 1280, H = 720, PAD = 24, GAP = 16;

void lock();
void unlock();
lv_obj_t *screen();                                  // new blank screen in the theme background
lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
void module_label(lv_obj_t *parent, const char *num, const char *name);
lv_obj_t *keycap(lv_obj_t *parent, const char *text, int w, int h, bool accent, lv_event_cb_t cb, void *ud);

/** A rotary dial: light disc, orange pointer, touch arc underneath, mono caption below. */
struct Dial {
    lv_obj_t *arc, *ptr, *lbl;
    int cx, cy;
    const char *caption;
    void (*on_change)(Dial *, int value, bool released);   ///< value while dragging; released = finger up
};
Dial *dial_create(lv_obj_t *parent, int x, int y, int size, const char *caption, int min, int max,
                  void (*on_change)(Dial *, int, bool));
void  dial_set(Dial *d, int value);                    ///< move the pointer and update the caption value
int   dial_value(Dial *d);
void  dial_caption(Dial *d, const char *text);         ///< override the caption line entirely
}
