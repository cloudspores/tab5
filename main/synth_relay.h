/**
 * @file synth_relay.h
 * @brief Sends the synth's live audio to a Sonos room through the bridge.
 *
 * The output is either the Tab5's own speaker/headphones or one Sonos room. For a room, the
 * bridge is asked to point the room at its live MP3 stream and the rendered audio is pushed
 * to the bridge over a WebSocket; the local speaker is muted meanwhile. Sonos buffers a
 * second or two of a stream, so the room lags the keys; the local output does not.
 */
#pragma once
#include <string>
#include <vector>

namespace synth_relay {

constexpr const char *LOCAL = "TAB5";

/** Restore the persisted output and start the worker. Call once per app entry (safe to repeat). */
void start();
/** Stop relaying and hand the room back (called when the synth app exits). */
void stop();

/** Ask the bridge for the Sonos rooms (asynchronous; outputs() grows when the answer arrives). */
void discover();
/** Output names: LOCAL first, then the rooms found. */
const std::vector<std::string> &outputs();
/** Select an output by name (LOCAL or a room); persisted. Asynchronous: the switch happens on the worker. */
void select(const char *name);
/** Select the next output in the list. */
void next();
const char *current();
/** Short state for the OUT module: "TAB5 HP", "SONOS Kitchen", "CONNECTING", ... */
const char *status();
/** Set the selected room's volume (no effect on the local output); applied by the worker. */
void set_volume(int percent);
/** Counter bumped whenever the output list or selection changes; the UI polls it. */
int changed_count();

}
