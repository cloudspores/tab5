/**
 * @file synth_play_ui.h
 * @brief The synth's PLAY screen: chord pads, keys, arpeggiator, scale, patch.
 *
 * Pure view: it renders the state of the performance layer (synth_perf) and reports touches
 * through the handlers below. The app owns the logic and calls refresh() after any change.
 */
#pragma once
#include "lvgl.h"
#include <cstdint>

namespace synth_play_ui {

enum Key {
    KEY_LATCH, KEY_VOICING, KEY_SPREAD, KEY_SEVENTHS,
    KEY_ARP_OFF, KEY_ARP_UP, KEY_ARP_DOWN, KEY_ARP_RANDOM,
    KEY_ROOT_DOWN, KEY_ROOT_UP, KEY_MODE, KEY_LOCK,
    KEY_PREV, KEY_NEXT, KEY_SOUND, KEY_OCT_DOWN, KEY_OCT_UP,
};
enum DialId { DIAL_RATE, DIAL_GATE, DIAL_TEMPO };

using KeyHandler  = void (*)(Key k);
using PadHandler  = void (*)(int degree, bool down);       ///< degree 0..6
using NoteHandler = void (*)(int midi_note, bool on);
using DialHandler = void (*)(DialId d, int value, bool released);

lv_obj_t *init();                                           ///< creates the screen once; LVGL locked by caller
void on_key(KeyHandler h);
void on_pad(PadHandler h);
void on_note(NoteHandler h);
void on_dial(DialHandler h);

/** Re-read the performance state (scale, pads, arp, toggles) and repaint. Takes the LVGL lock. */
void refresh();
void set_patch(const char *name, int index, int count);
void set_octave(int base_midi_note);
/** Light sounding notes on the keyboard and the active pad. Safe from any task. */
void highlight(uint32_t key_mask, int active_pad);

}
