// An app owns one LVGL screen. Audio/network work may keep running after exit (the radio does).
#pragma once
#include "lvgl.h"

struct App {
    const char *id;        // "radio", used in settings and the console
    const char *name;      // card title
    const char *desc;      // card subtitle
    const char *icon;      // LVGL symbol or nullptr
    lv_obj_t *(*screen)(); // returns the app's screen, created on first call (LVGL locked by caller)
    void (*enter)();       // called after the screen is shown
    void (*exit)();        // called before leaving
    const char *(*status)();   // short live text for the card, e.g. "PLAYING", or nullptr
};
