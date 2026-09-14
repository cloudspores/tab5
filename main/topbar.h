// Shared top bar: product name (tap = home), status readout, output, WiFi, clock.
// Each screen creates its own instance; setters update every instance.
#pragma once
#include "lvgl.h"

namespace topbar {
constexpr int HEIGHT = 32;      // plus the page padding above it
void create(lv_obj_t *screen, const char *app_name);
void on_home(void (*handler)());
void set_status(const char *text, bool live);   // live = orange dot
void set_output(const char *name);
void set_wifi(const char *text);
void set_clock(const char *text);
}
