/**
 * @file synth_ui.cpp
 * @brief SOUND screen in the instrument style: 01 SOUND, 02 ALGORITHM, 03 OUT, 04 KEYS.
 *
 * The keyboard widget is shared with the PLAY screen (synth_keys).
 */
#include "synth_ui.h"
#include "theme.h"
#include "topbar.h"
#include "synth_keys.h"
#include <cstdio>
#include <cstring>
#include "esp_timer.h"

namespace {
using namespace theme;

lv_obj_t *scr = nullptr;
lv_obj_t *lbl_name, *lbl_index, *lbl_algo, *lbl_fb, *lbl_status, *lbl_keys_cap, *meter_bars[30];
char notice_text[40]; int64_t notice_until = 0;      ///< short message shown in the OUT readout instead of the status
Dial *dials[4], *vol_dial;
lv_obj_t *out_panel; lv_obj_t *out_keys[synth_ui::MAX_OUTPUTS]; int n_out_keys = 0; int out_selected = -1;
constexpr int OUT_LIST_Y = 76, OUT_KEY_H = 30, OUT_KEY_W = 250;
synth_keys::Keyboard *keys;
synth_ui::KeyHandler key_h; synth_ui::NoteHandler note_h; synth_ui::MacroHandler macro_h;
synth_ui::OutputHandler output_h; synth_ui::VolumeHandler volume_h;

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && key_h) key_h((synth_ui::Key)(intptr_t)lv_event_get_user_data(e));
}

void dial_cb(Dial *d, int v, bool released)
{
    if (d == vol_dial) { if (volume_h) volume_h(v, released); return; }
    for (int i = 0; i < 4; i++) if (dials[i] == d && macro_h) macro_h((synth_ui::Macro)i, v, released);
}

/** Output radio buttons: one keycap per output, the selected one in the accent colour. */
void output_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && output_h) output_h((int)(intptr_t)lv_event_get_user_data(e));
}

/** Rebuild the output list (LVGL locked by caller). Names are shown in capitals, like the readouts. */
void set_outputs_locked(const char *const *names, int n, int selected)
{
    for (int i = 0; i < n_out_keys; i++) lv_obj_delete(out_keys[i]);
    n_out_keys = 0;
    for (int i = 0; i < n && i < synth_ui::MAX_OUTPUTS; i++) {
        char t[28]; snprintf(t, sizeof t, "%.24s", names[i]);
        for (char *c = t; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        lv_obj_t *k = keycap(out_panel, t, OUT_KEY_W, OUT_KEY_H, i == selected, output_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(k, 0, OUT_LIST_Y + i * (OUT_KEY_H + 8));
        lv_obj_align(lv_obj_get_child(k, 0), LV_ALIGN_LEFT_MID, 12, 0);
        out_keys[n_out_keys++] = k;
    }
    out_selected = selected;
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
    // The row must be wide enough for every key and must not scroll: a scrollable parent turns
    // the slightest finger movement into a drag and swallows the key's click.
    lv_obj_set_size(row, 340, 44); lv_obj_align(row, LV_ALIGN_TOP_RIGHT, 0, 26);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(row, 8, 0);
    keycap(row, LV_SYMBOL_LEFT, 44, 44, false, key_cb, (void *)KEY_PREV);
    keycap(row, LV_SYMBOL_RIGHT, 44, 44, false, key_cb, (void *)KEY_NEXT);
    keycap(row, "RANDOM", 84, 44, true, key_cb, (void *)KEY_RANDOM);
    keycap(row, "PANIC", 56, 44, false, key_cb, (void *)KEY_PANIC);
    keycap(row, "PLAY " LV_SYMBOL_RIGHT, 68, 44, false, key_cb, (void *)KEY_PLAY);
    lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(row, 0), 0), &lv_font_montserrat_14, 0);   // arrow glyphs
    lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(row, 1), 0), &lv_font_montserrat_14, 0);
    static const char *names[4] = {"BRIGHT", "ATTACK", "RELEASE", "MOTION"};
    const int dsize = 96, dy = sound_h - 36 - dsize - 30;
    for (int i = 0; i < 4; i++) dials[i] = dial_create(p, 20 + i * ((left_w - 36 - 40) / 4) + 10, dy, dsize, names[i], 0, 100, dial_cb);

    // ---- right column: 02 ALGORITHM, 03 OUT ----
    const int rh = (sound_h - 2 * GAP) / 3;
    const int rx = PAD + left_w + GAP;
    lv_obj_t *pa = panel(scr, rx, top, right_w, rh);
    module_label(pa, "02", "ALGORITHM");
    lbl_algo = label(pa, "--", &familjen_bold_52, INK); lv_obj_align(lbl_algo, LV_ALIGN_LEFT_MID, 0, 8);
    lbl_fb = label(pa, "", &jbmono_14, MID); lv_obj_align(lbl_fb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *kx = keycap(pa, "EXPERT", 84, 36, false, key_cb, (void *)KEY_EXPERT); lv_obj_align(kx, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // OUT takes the rest of the column: level meter, output radio buttons, volume dial
    out_panel = panel(scr, rx, top + rh + GAP, right_w, sound_h - rh - GAP);
    module_label(out_panel, "03", "OUT");
    lbl_status = label(out_panel, "TAB5 HP . 0 VOICES", &jbmono_14, MID); lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 0, 26);
    const int bw = 8, bgap = 4;
    for (int i = 0; i < 30; i++) {
        lv_obj_t *b = lv_obj_create(out_panel);
        lv_obj_set_size(b, bw, 6);
        lv_obj_set_pos(b, i * (bw + bgap), 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(LIGHT), 0);
        lv_obj_set_style_border_width(b, 0, 0); lv_obj_set_style_radius(b, 2, 0); lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        meter_bars[i] = b;
    }
    const int vsize = 96;
    vol_dial = dial_create(out_panel, right_w - 36 - vsize - 6, OUT_LIST_Y + 20, vsize, "VOL", 0, 100, dial_cb);
    const char *local[1] = {"TAB5"};
    set_outputs_locked(local, 1, 0);

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
void on_output(OutputHandler h) { output_h = h; }
void on_volume(VolumeHandler h) { volume_h = h; }

void set_outputs(const char *const *names, int n, int selected) { lock(); set_outputs_locked(names, n, selected); unlock(); }

void set_volume(int percent) { lock(); dial_set(vol_dial, percent); unlock(); }

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
    if (esp_timer_get_time() < notice_until) lv_label_set_text(lbl_status, notice_text);
    else lv_label_set_text_fmt(lbl_status, "%s . %d VOICES", output, voices);
    unlock();
}

void notice(const char *text)
{
    strlcpy(notice_text, text, sizeof notice_text);
    notice_until = esp_timer_get_time() + 1500 * 1000;
}

void set_status(const char *t) { lock(); lv_label_set_text(lbl_status, t); unlock(); }

void highlight(uint32_t mask) { lock(); synth_keys::highlight(keys, mask); unlock(); }

}
