// Instrument-panel UI for the Tab5 radio (LVGL 9). Setters lock LVGL internally.
// Handlers run in the LVGL task: keep them short (post to a queue).
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "lvgl.h"

namespace ui {

enum Key { KEY_VOL_DOWN, KEY_VOL_UP, KEY_MUTE, KEY_OUTPUT, KEY_SEARCH, KEY_CR, KEY_HOME, KEY_NEXT, KEY_LIGHT, KEY_PLAY, KEY_POWER };
enum Dial { DIAL_TUNE = 0, DIAL_VOL = 1 };
using KeyHandler    = void (*)(Key k);
using IndexHandler  = void (*)(int index);
using PresetHandler = void (*)(int slot, bool store);
using TextHandler   = void (*)(const char *text);
using DialHandler   = void (*)(int dial, int value);

lv_obj_t *init();               // creates (once) and returns the radio screen; caller holds the LVGL lock

void on_key(KeyHandler h);          // control keys, output selector, band keys, station tap (= next)
void on_tune(IndexHandler h);       // tap on a band entry
void on_preset(PresetHandler h);    // tap (play) / long press (store) on a preset key
void on_search(TextHandler h);      // search text submitted from the overlay keyboard
void on_result(IndexHandler h);     // tap on a search result
void on_dial(DialHandler h);        // rotary dial released (TUNE) / moved (VOL)

void set_status(const char *text);  // top-right readout; "ON AIR" lights the dot
void set_clock(const char *text);
void set_output(const char *name);
void set_station(const char *name);
void set_title(const char *text);
void set_format(const char *codec, int kbps, int sample_rate, int channels);
void set_volume(int percent, bool muted);
void set_brightness_text(int percent);
void set_playing(bool playing);     // play/pause key face
void set_band(const std::vector<std::string> &names, int current);
void set_band_current(int current);
void set_presets(const std::vector<std::string> &names);
void show_results(const std::vector<std::string> &names);   // fills the overlay list
void open_search();
void close_search();
void push_level(uint8_t level_0_100);

}
