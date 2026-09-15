/**
 * @file synth_play_ui.cpp
 * @brief PLAY screen in the instrument style: 01 CHORDS, 02 KEYS, 03 ARP, 04 SCALE, 05 PATCH.
 */
#include "synth_play_ui.h"
#include "synth_perf.h"
#include "synth_keys.h"
#include "theme.h"
#include "topbar.h"
#include <cstdio>
#include <cstring>

namespace {
using namespace theme;

lv_obj_t *scr = nullptr;
lv_obj_t *pad[perf::PADS + 1], *pad_num[perf::PADS + 1], *pad_sym[perf::PADS + 1];
lv_obj_t *k_latch, *k_voicing, *k_spread, *k_arp[4], *k_mode, *k_lock;
lv_obj_t *lbl_chords_cap, *lbl_scale, *lbl_patch, *lbl_patch_idx, *lbl_keys_cap;
Dial *d_rate, *d_gate, *d_tempo;
synth_keys::Keyboard *keys;
int active_pad_shown = -1;

synth_play_ui::KeyHandler key_h; synth_play_ui::PadHandler pad_h;
synth_play_ui::NoteHandler note_h; synth_play_ui::DialHandler dial_h;

void key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && key_h) key_h((synth_play_ui::Key)(intptr_t)lv_event_get_user_data(e));
}

/** Chord pads act like keys: down on press, up on release or when the finger leaves. */
void pad_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int degree = (int)(intptr_t)lv_event_get_user_data(e);
    if (degree == perf::PADS) {                                // the eighth pad toggles sevenths
        if (c == LV_EVENT_CLICKED && key_h) key_h(synth_play_ui::KEY_SEVENTHS);
        return;
    }
    if (!pad_h) return;
    if (c == LV_EVENT_PRESSED) pad_h(degree, true);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) pad_h(degree, false);
}

void dial_cb(Dial *d, int v, bool released)
{
    if (!dial_h) return;
    dial_h(d == d_rate ? synth_play_ui::DIAL_RATE : d == d_gate ? synth_play_ui::DIAL_GATE : synth_play_ui::DIAL_TEMPO, v, released);
}

void paint_pad(int i, bool on)
{
    lv_obj_set_style_bg_color(pad[i], lv_color_hex(on ? ORANGE : DISC), 0);
    lv_obj_set_style_border_color(pad[i], lv_color_hex(on ? ORANGE : LIGHT), 0);
    lv_obj_set_style_text_color(pad_num[i], lv_color_hex(on ? 0xffffff : MID), 0);
    lv_obj_set_style_text_color(pad_sym[i], lv_color_hex(on ? 0xffffff : INK), 0);
}

lv_obj_t *make_pad(lv_obj_t *parent, int x, int y, int w, int h, int degree)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(DISC), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    pad_num[degree] = label(b, "", &jbmono_14, MID); lv_obj_align(pad_num[degree], LV_ALIGN_TOP_LEFT, 0, 0);
    pad_sym[degree] = label(b, "", &familjen_medium_24, INK); lv_obj_align(pad_sym[degree], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b, pad_cb, LV_EVENT_ALL, (void *)(intptr_t)degree);
    return b;
}

void refresh_locked()
{
    // pads: names for the current scale, the eighth pad shows the sevenths toggle
    for (int i = 0; i < perf::PADS; i++) {
        char num[8], sym[12];
        perf::chord_label(i, num, sym);
        lv_label_set_text(pad_num[i], num);
        lv_label_set_text(pad_sym[i], sym);
        paint_pad(i, i == perf::active_pad());
    }
    lv_label_set_text(pad_num[perf::PADS], "+");
    lv_label_set_text(pad_sym[perf::PADS], "7ths");
    paint_pad(perf::PADS, perf::sevenths());
    active_pad_shown = perf::active_pad();
    char cap[64];
    snprintf(cap, sizeof cap, "%s . TAP A PAD, PLAY ONE FINGER", perf::scale_name());
    for (char *c = cap; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(lbl_chords_cap, cap);
    // toggles
    keycap_accent(k_latch, perf::latch());
    char v[16]; snprintf(v, sizeof v, "VOICING %d", perf::voicing()); keycap_text(k_voicing, v);
    keycap_accent(k_spread, perf::spread());
    for (int i = 0; i < 4; i++) keycap_accent(k_arp[i], perf::arp() == (perf::Arp)i);
    // scale
    lv_label_set_text(lbl_scale, perf::scale_name());
    char m[16]; snprintf(m, sizeof m, "%s", perf::mode_name(perf::mode())); for (char *c = m; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    keycap_text(k_mode, m);
    keycap_accent(k_lock, perf::scale_lock());
    // dials
    char t[24];
    dial_set(d_rate, perf::rate());  snprintf(t, sizeof t, "RATE %s", perf::rate_name(perf::rate())); dial_caption(d_rate, t);
    dial_set(d_gate, perf::gate());  snprintf(t, sizeof t, "GATE %d%%", perf::gate()); dial_caption(d_gate, t);
    dial_set(d_tempo, perf::tempo()); snprintf(t, sizeof t, "TEMPO %d", perf::tempo()); dial_caption(d_tempo, t);
}
} // namespace

namespace synth_play_ui {

lv_obj_t *init()
{
    if (scr) return scr;
    scr = screen();
    topbar::create(scr, "fm synth");
    const int top = PAD + topbar::HEIGHT + GAP;
    const int left_w = 740, right_w = W - 2 * PAD - left_w - GAP;
    const int keys_h = 190;
    const int chords_h = H - top - PAD - GAP - keys_h;
    const int inner_w = left_w - 36;

    // ---- 01 CHORDS ----
    lv_obj_t *pc = panel(scr, PAD, top, left_w, chords_h);
    module_label(pc, "01", "CHORDS");
    lbl_chords_cap = label(pc, "", &jbmono_14, MID); lv_obj_align(lbl_chords_cap, LV_ALIGN_TOP_RIGHT, 0, 0);
    const int pw = (inner_w - 3 * 12) / 4, ph = 100;
    for (int i = 0; i <= perf::PADS; i++) pad[i] = make_pad(pc, (i % 4) * (pw + 12), 30 + (i / 4) * (ph + 12), pw, ph, i);
    const int ky = 30 + 2 * (ph + 12) + 6;
    k_latch   = keycap(pc, "LATCH", 72, 36, false, key_cb, (void *)KEY_LATCH);      lv_obj_set_pos(k_latch, 0, ky);
    k_voicing = keycap(pc, "VOICING 1", 100, 36, false, key_cb, (void *)KEY_VOICING); lv_obj_set_pos(k_voicing, 80, ky);
    k_spread  = keycap(pc, "SPREAD", 76, 36, false, key_cb, (void *)KEY_SPREAD);    lv_obj_set_pos(k_spread, 188, ky);

    // ---- 02 KEYS ----
    lv_obj_t *pk = panel(scr, PAD, top + chords_h + GAP, left_w, keys_h);
    module_label(pk, "02", "KEYS");
    lbl_keys_cap = label(pk, "C3 - B4", &jbmono_14, MID); lv_obj_align(lbl_keys_cap, LV_ALIGN_TOP_RIGHT, -120, 0);
    lv_obj_t *kd = keycap(pk, "OCT -", 52, 24, false, key_cb, (void *)KEY_OCT_DOWN); lv_obj_align(kd, LV_ALIGN_TOP_RIGHT, -56, -4);
    lv_obj_t *ku = keycap(pk, "OCT +", 52, 24, false, key_cb, (void *)KEY_OCT_UP);   lv_obj_align(ku, LV_ALIGN_TOP_RIGHT, 0, -4);
    keys = synth_keys::create(pk, 0, 28, inner_w, keys_h - 36 - 28, 48, [](int n, bool on) { if (note_h) note_h(n, on); });

    // ---- right column ----
    const int rx = PAD + left_w + GAP;
    const int rh = (H - top - PAD - 2 * GAP) / 3;

    lv_obj_t *pa = panel(scr, rx, top, right_w, rh);
    module_label(pa, "03", "ARP");
    static const char *arp_names[4] = {"OFF", "UP", "DOWN", "RND"};
    static const int arp_w[4] = {50, 44, 58, 50};
    for (int i = 0, x = 0; i < 4; x += arp_w[i] + 8, i++) { k_arp[i] = keycap(pa, arp_names[i], arp_w[i], 32, false, key_cb, (void *)(intptr_t)(KEY_ARP_OFF + i)); lv_obj_set_pos(k_arp[i], x, 24); }
    const int ds = 66, dy = 64, dgap = (right_w - 36 - 3 * ds) / 2;
    d_rate  = dial_create(pa, 0, dy, ds, "RATE", 0, perf::RATES - 1, dial_cb);
    d_gate  = dial_create(pa, ds + dgap, dy, ds, "GATE", 10, 100, dial_cb);
    d_tempo = dial_create(pa, 2 * (ds + dgap), dy, ds, "TEMPO", 40, 240, dial_cb);

    lv_obj_t *ps = panel(scr, rx, top + rh + GAP, right_w, rh);
    module_label(ps, "04", "SCALE");
    lbl_scale = label(ps, "C major", &familjen_bold_52, INK); lv_obj_align(lbl_scale, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_t *r1 = keycap(ps, "ROOT " LV_SYMBOL_LEFT, 74, 32, false, key_cb, (void *)KEY_ROOT_DOWN); lv_obj_align(r1, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *r2 = keycap(ps, "ROOT " LV_SYMBOL_RIGHT, 74, 32, false, key_cb, (void *)KEY_ROOT_UP); lv_obj_align(r2, LV_ALIGN_BOTTOM_LEFT, 82, 0);
    k_mode = keycap(ps, "MAJOR", 108, 32, false, key_cb, (void *)KEY_MODE); lv_obj_align(k_mode, LV_ALIGN_BOTTOM_LEFT, 164, 0);
    k_lock = keycap(ps, "LOCK", 56, 32, false, key_cb, (void *)KEY_LOCK); lv_obj_align(k_lock, LV_ALIGN_BOTTOM_LEFT, 280, 0);

    lv_obj_t *pp = panel(scr, rx, top + 2 * (rh + GAP), right_w, H - PAD - (top + 2 * (rh + GAP)));
    module_label(pp, "05", "PATCH");
    lbl_patch = label(pp, "--", &familjen_medium_24, INK); lv_obj_align(lbl_patch, LV_ALIGN_TOP_LEFT, 0, 28);
    lbl_patch_idx = label(pp, "", &jbmono_14, MID); lv_obj_align(lbl_patch_idx, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_t *p1 = keycap(pp, LV_SYMBOL_LEFT, 44, 32, false, key_cb, (void *)KEY_PREV); lv_obj_align(p1, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *p2 = keycap(pp, LV_SYMBOL_RIGHT, 44, 32, false, key_cb, (void *)KEY_NEXT); lv_obj_align(p2, LV_ALIGN_BOTTOM_LEFT, 52, 0);
    lv_obj_t *p3 = keycap(pp, "SOUND " LV_SYMBOL_RIGHT, 96, 32, true, key_cb, (void *)KEY_SOUND); lv_obj_align(p3, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    refresh_locked();
    return scr;
}

void on_key(KeyHandler h)   { key_h = h; }
void on_pad(PadHandler h)   { pad_h = h; }
void on_note(NoteHandler h) { note_h = h; }
void on_dial(DialHandler h) { dial_h = h; }

void refresh() { if (!scr) return; lock(); refresh_locked(); unlock(); }

void set_patch(const char *name, int index, int count)
{
    if (!scr) return;
    lock();
    lv_label_set_text(lbl_patch, name);
    lv_label_set_text_fmt(lbl_patch_idx, "VOICE %02d / %02d", index + 1, count);
    unlock();
}

void set_octave(int base)
{
    if (!scr) return;
    static const char *nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    lock();
    synth_keys::set_base(keys, base);
    lv_label_set_text_fmt(lbl_keys_cap, "%s%d - %s%d", nn[base % 12], base / 12 - 1, nn[(base + 23) % 12], (base + 23) / 12 - 1);
    unlock();
}

void highlight(uint32_t mask, int active)
{
    if (!scr) return;
    lock();
    synth_keys::highlight(keys, mask);
    if (active != active_pad_shown) {
        if (active_pad_shown >= 0) paint_pad(active_pad_shown, false);
        if (active >= 0) paint_pad(active, true);
        active_pad_shown = active;
    }
    unlock();
}

}
