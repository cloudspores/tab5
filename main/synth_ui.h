/**
 * @file synth_ui.h
 * @brief The synth's SOUND screen: patch, macro dials, algorithm, output, touch keyboard.
 */
#pragma once
#include "lvgl.h"
#include <cstdint>

namespace synth_ui {

enum Key { KEY_PREV, KEY_NEXT, KEY_RANDOM, KEY_FAV, KEY_EXPERT, KEY_OCT_DOWN, KEY_OCT_UP, KEY_PANIC, KEY_PLAY };
constexpr int MAX_OUTPUTS = 6;                       // TAB5 plus up to five Sonos rooms
enum Macro { MACRO_BRIGHT = 0, MACRO_ATTACK, MACRO_RELEASE, MACRO_MOTION };

using KeyHandler   = void (*)(Key k);
using NoteHandler  = void (*)(int midi_note, bool on);
using MacroHandler = void (*)(Macro m, int value, bool released);
using OutputHandler = void (*)(int index);            // an output radio button was tapped
using VolumeHandler = void (*)(int percent, bool released);

lv_obj_t *init();                                     // creates the screen once; LVGL locked by caller
void on_key(KeyHandler h);
void on_note(NoteHandler h);
void on_macro(MacroHandler h);
void on_output(OutputHandler h);
void on_volume(VolumeHandler h);

void set_patch(const char *name, int index, int count, int algorithm, int feedback);
void set_macro(Macro m, int value);
void set_octave(int base_midi_note);                  // lowest key of the on-screen keyboard
void set_meter(int peak_0_32767, int voices, const char *output);   // output: "TAB5 HP", "SONOS KITCHEN", ...
void set_status(const char *text);
void notice(const char *text);                        // show a message in the OUT readout for 1.5 s (key feedback)
void set_outputs(const char *const *names, int count, int selected);   // rebuild the output radio buttons
void set_volume(int percent);
void highlight(uint32_t sounding_mask);                 // light the keys that sound (bit n = lowest key + n)

}
