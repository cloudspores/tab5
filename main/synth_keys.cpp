/**
 * @file synth_keys.cpp
 * @brief Two-octave touch keyboard: 14 white keys with 10 black keys overlaid.
 */
#include "synth_keys.h"
#include "theme.h"

namespace synth_keys {

struct Keyboard {
    lv_obj_t *key[SPAN];          ///< indexed by semitone above base
    int base;
    uint32_t lit;                 ///< current highlight mask
    NoteHandler handler;
};

namespace {
using namespace theme;

const int WHITE_SEMI[7] = {0, 2, 4, 5, 7, 9, 11};
const int BLACK_SEMI[5] = {1, 3, 6, 8, 10};
const int BLACK_AFTER[5] = {0, 1, 3, 4, 5};                ///< black key sits after this white key of the octave

bool is_black(int semi) { int s = semi % 12; return s == 1 || s == 3 || s == 6 || s == 8 || s == 10; }

struct KeyRef { Keyboard *kb; int semi; };

void note_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    KeyRef *r = (KeyRef *)lv_event_get_user_data(e);
    if (!r->kb->handler) return;
    if (c == LV_EVENT_PRESSED) r->kb->handler(r->kb->base + r->semi, true);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) r->kb->handler(r->kb->base + r->semi, false);
}

void paint(Keyboard *k, int semi)
{
    bool on = (k->lit >> semi) & 1;
    uint32_t colour = on ? ORANGE : is_black(semi) ? INK : 0xffffff;
    lv_obj_set_style_bg_color(k->key[semi], lv_color_hex(colour), 0);
}
} // namespace

Keyboard *create(lv_obj_t *parent, int x, int y, int w, int h, int base_note, NoteHandler handler)
{
    Keyboard *k = new Keyboard{};
    k->base = base_note; k->handler = handler;
    const float wk = (float)w / 14.0f;
    for (int i = 0; i < 14; i++) {
        int semi = 12 * (i / 7) + WHITE_SEMI[i % 7];
        lv_obj_t *b = lv_button_create(parent);
        lv_obj_set_pos(b, x + (int)(i * wk), y); lv_obj_set_size(b, (int)wk - 3, h);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(ORANGE), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(LIGHT), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, note_cb, LV_EVENT_ALL, new KeyRef{k, semi});
        k->key[semi] = b;
    }
    for (int o = 0; o < 2; o++)
        for (int bi = 0; bi < 5; bi++) {
            int semi = 12 * o + BLACK_SEMI[bi];
            lv_obj_t *b = lv_button_create(parent);
            lv_obj_set_pos(b, x + (int)((o * 7 + BLACK_AFTER[bi] + 1) * wk - wk * 0.3f), y);
            lv_obj_set_size(b, (int)(wk * 0.6f), (int)(h * 0.6f));
            lv_obj_set_style_bg_color(b, lv_color_hex(INK), 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(ORANGE), LV_STATE_PRESSED);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_add_event_cb(b, note_cb, LV_EVENT_ALL, new KeyRef{k, semi});
            k->key[semi] = b;
        }
    return k;
}

void set_base(Keyboard *k, int base_note) { k->base = base_note; }
int  base(Keyboard *k) { return k->base; }

void highlight(Keyboard *k, uint32_t mask)
{
    uint32_t diff = mask ^ k->lit;
    k->lit = mask;
    for (int s = 0; s < SPAN; s++) if ((diff >> s) & 1) paint(k, s);
}

}
