/**
 * @file synth_ui.cpp
 * @brief SOUND screen in the instrument style: 01 SOUND, 02 ALGORITHM, 03 EFFECTS, 04 KEYS, 05 OUT.
 *
 * The keyboard widget is shared with the PLAY screen (synth_keys).
 */
#include "synth_ui.h"
#include "theme.h"
#include "topbar.h"
#include "synth_keys.h"
#include <cstdio>
#include <cstring>

namespace {
using namespace theme;

lv_obj_t *scr = nullptr;
lv_obj_t *lbl_name, *lbl_index, *lbl_algo, *lbl_fb, *lbl_status, *lbl_keys_cap, *meter_bars[30];
Dial *dials[4];
synth_keys::Keyboard *keys;
synth_ui::KeyHandler key_h; synth_ui::NoteHandler note_h; synth_ui::MacroHandler macro_h;

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && key_h) key_h((synth_ui::Key)(intptr_t)lv_event_get_user_data(e));
}

void dial_cb(Dial *d, int v, bool released)
{
    for (int i = 0; i < 4; i++) if (dials[i] == d && macro_h) macro_h((synth_ui::Macro)i, v, released);
}
}

namespace synth_ui {

lv_obj_t *init()
{
    if (scr) return scr;
    scr = screen();
    topbar::create(scr, "fm synth");
    const int top = PAD + topbar::HEIGHT + GAP;
    const int left_w = 740, right_w = W - 2 * PAD - left_w - GAP;
    const int keys_h = 150;
    const int sound_h = H - top - PAD - GAP - keys_h;

    // ---- 01 SOUND ----
    lv_obj_t *p = panel(scr, PAD, top, left_w, sound_h);
    module_label(p, "01", "SOUND");
    lbl_name = label(p, "--", &familjen_bold_52, INK);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT); lv_obj_set_width(lbl_name, left_w - 36 - 330);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, 30);
    lbl_index = label(p, "", &jbmono_14, MID);
    lv_obj_align(lbl_index, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 44); lv_obj_align(row, LV_ALIGN_TOP_RIGHT, 0, 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(row, 8, 0);
    keycap(row, LV_SYMBOL_LEFT, 44, 44, false, key_cb, (void *)KEY_PREV);
    keycap(row, LV_SYMBOL_RIGHT, 44, 44, false, key_cb, (void *)KEY_NEXT);
    keycap(row, "RANDOM", 84, 44, true, key_cb, (void *)KEY_RANDOM);
    keycap(row, "PANIC", 56, 44, false, key_cb, (void *)KEY_PANIC);
    keycap(row, "PLAY " LV_SYMBOL_RIGHT, 68, 44, false, key_cb, (void *)KEY_PLAY);
    for (lv_obj_t *k = lv_obj_get_child(row, 0); k; k = nullptr) {
        lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(row, 0), 0), &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(row, 1), 0), &lv_font_montserrat_14, 0);
    }
    static const char *names[4] = {"BRIGHT", "ATTACK", "RELEASE", "MOTION"};
    const int dsize = 96, dy = sound_h - 36 - dsize - 30;
    for (int i = 0; i < 4; i++) dials[i] = dial_create(p, 20 + i * ((left_w - 36 - 40) / 4) + 10, dy, dsize, names[i], 0, 100, dial_cb);

    // ---- right column: 02 ALGORITHM, 03 EFFECTS, 05 OUT ----
    const int rh = (sound_h - 2 * GAP) / 3;
    const int rx = PAD + left_w + GAP;
    lv_obj_t *pa = panel(scr, rx, top, right_w, rh);
    module_label(pa, "02", "ALGORITHM");
    lbl_algo = label(pa, "--", &familjen_bold_52, INK); lv_obj_align(lbl_algo, LV_ALIGN_LEFT_MID, 0, 8);
    lbl_fb = label(pa, "", &jbmono_14, MID); lv_obj_align(lbl_fb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *kx = keycap(pa, "EXPERT", 84, 36, false, key_cb, (void *)KEY_EXPERT); lv_obj_align(kx, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *pe = panel(scr, rx, top + rh + GAP, right_w, rh);
    module_label(pe, "03", "EFFECTS");
    lv_obj_t *fx = label(pe, "DRY . EFFECTS COME NEXT", &jbmono_14, MID); lv_obj_align(fx, LV_ALIGN_LEFT_MID, 0, 6);

    lv_obj_t *po = panel(scr, rx, top + 2 * (rh + GAP), right_w, sound_h - 2 * (rh + GAP));
    module_label(po, "05", "OUT");
    lbl_status = label(po, "TAB5 HP . 0 VOICES", &jbmono_14, MID); lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_t *ko = keycap(po, "OUT", 52, 28, false, key_cb, (void *)KEY_OUT); lv_obj_align(ko, LV_ALIGN_TOP_RIGHT, 0, -6);
    const int bw = 8, bgap = 4;
    for (int i = 0; i < 30; i++) {
        lv_obj_t *b = lv_obj_create(po);
        lv_obj_set_size(b, bw, 6);
        lv_obj_set_pos(b, i * (bw + bgap), rh - 36 - 6 - 12);
        lv_obj_set_style_bg_color(b, lv_color_hex(LIGHT), 0);
        lv_obj_set_style_border_width(b, 0, 0); lv_obj_set_style_radius(b, 2, 0); lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        meter_bars[i] = b;
    }

    // ---- 04 KEYS ----
    lv_obj_t *pk = panel(scr, PAD, top + sound_h + GAP, W - 2 * PAD, keys_h);
    module_label(pk, "04", "KEYS");
    lbl_keys_cap = label(pk, "C3 - B4", &jbmono_14, MID); lv_obj_align(lbl_keys_cap, LV_ALIGN_TOP_RIGHT, -120, 0);
    lv_obj_t *kd = keycap(pk, "OCT -", 52, 24, false, key_cb, (void *)KEY_OCT_DOWN); lv_obj_align(kd, LV_ALIGN_TOP_RIGHT, -56, -4);
    lv_obj_t *ku = keycap(pk, "OCT +", 52, 24, false, key_cb, (void *)KEY_OCT_UP);   lv_obj_align(ku, LV_ALIGN_TOP_RIGHT, 0, -4);
    keys = synth_keys::create(pk, 0, 28, W - 2 * PAD - 36, keys_h - 36 - 28, 48, [](int n, bool on) { if (note_h) note_h(n, on); });
    return scr;
}

void on_key(KeyHandler h)     { key_h = h; }
void on_note(NoteHandler h)   { note_h = h; }
void on_macro(MacroHandler h) { macro_h = h; }

void set_patch(const char *name, int index, int count, int algorithm, int fb)
{
    lock();
    lv_label_set_text(lbl_name, name);
    lv_label_set_text_fmt(lbl_index, "VOICE %02d / %02d", index + 1, count);
    lv_label_set_text_fmt(lbl_algo, "%02d", algorithm);
    lv_label_set_text_fmt(lbl_fb, "FEEDBACK %d . 6 OPS", fb);
    unlock();
}

void set_macro(Macro m, int value) { lock(); dial_set(dials[m], value); unlock(); }

void set_octave(int base)
{
    static const char *nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    lock();
    synth_keys::set_base(keys, base);
    lv_label_set_text_fmt(lbl_keys_cap, "%s%d - %s%d", nn[base % 12], base / 12 - 1, nn[(base + 23) % 12], (base + 23) / 12 - 1);
    unlock();
}

void set_meter(int peak, int voices, const char *output)
{
    int lit = peak * 30 / 32767;
    lock();
    for (int i = 0; i < 30; i++) lv_obj_set_style_bg_color(meter_bars[i], lv_color_hex(i < lit ? (i >= 26 ? ORANGE : INK) : LIGHT), 0);
    lv_label_set_text_fmt(lbl_status, "%s . %d VOICES", output, voices);
    unlock();
}

void set_status(const char *t) { lock(); lv_label_set_text(lbl_status, t); unlock(); }

void highlight(uint32_t mask) { lock(); synth_keys::highlight(keys, mask); unlock(); }

}
