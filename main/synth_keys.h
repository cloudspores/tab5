/**
 * @file synth_keys.h
 * @brief Two-octave touch keyboard widget shared by the synth's screens.
 *
 * White and black keys are LVGL buttons; a key sends note-on when pressed and note-off when
 * released or when the finger slides off it. Keys can be lit in the accent colour to show
 * which notes are sounding (chords, arpeggiator steps).
 */
#pragma once
#include "lvgl.h"
#include <cstdint>

namespace synth_keys {

constexpr int SPAN = 24;                                   ///< semitones on the keyboard

struct Keyboard;
using NoteHandler = void (*)(int midi_note, bool on);

/** Build the keyboard inside `parent` at x,y with the given size; `base` is the lowest note. */
Keyboard *create(lv_obj_t *parent, int x, int y, int w, int h, int base_note, NoteHandler handler);
void set_base(Keyboard *k, int base_note);                 ///< transpose (octave keys); LVGL locked by caller
int  base(Keyboard *k);
/** Light the keys in `mask` (bit n = base + n); pressed keys stay highlighted anyway. LVGL locked by caller. */
void highlight(Keyboard *k, uint32_t mask);

}
