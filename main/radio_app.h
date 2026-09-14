/**
 * @file radio_app.h
 * @brief The internet radio app: stations, playback, outputs, presets, and its console commands.
 */
#pragma once
#include "app.h"

/** Launcher descriptor for the radio. */
extern const App radio_app;

namespace radio {

/** Create the radio screen, restore persisted state, register console commands, start the control task. */
void init();

/** Called once the network is up: discovers Sonos rooms via the bridge, then starts playback. */
void start();

}
