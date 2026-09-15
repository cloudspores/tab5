/**
 * @file synth_ui.h
 * @brief The synth's SOUND screen: patch, macro dials, algorithm, output, touch keyboard.
 */
#pragma once
#include "lvgl.h"

namespace synth_ui {

enum Key { KEY_PREV, KEY_NEXT, KEY_RANDOM, KEY_FAV, KEY_EXPERT, KEY_OCT_DOWN, KEY_OCT_UP, KEY_PANIC };
enum Macro { MACRO_BRIGHT = 0, MACRO_ATTACK, MACRO_RELEASE, MACRO_MOTION };

using KeyHandler   = void (*)(Key k);
using NoteHandler  = void (*)(int midi_note, bool on);
using MacroHandler = void (*)(Macro m, int value, bool released);

lv_obj_t *init();                                     // creates the screen once; LVGL locked by caller
void on_key(KeyHandler h);
void on_note(NoteHandler h);
void on_macro(MacroHandler h);

void set_patch(const char *name, int index, int count, int algorithm, int feedback);
void set_macro(Macro m, int value);
void set_octave(int base_midi_note);                  // lowest key of the on-screen keyboard
void set_meter(int peak_0_32767, int voices);
void set_status(const char *text);

}
