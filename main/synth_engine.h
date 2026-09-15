/**
 * @file synth_engine.h
 * @brief Six-operator FM synthesizer engine wrapper (msfa core) rendering to the speaker.
 *
 * The engine renders on core 1 in fixed-size blocks; everything else talks to it through
 * MIDI-style messages, so a USB or serial MIDI input later needs no new plumbing.
 */
#pragma once
#include <cstdint>
#include <cstddef>

namespace synth {

constexpr int SAMPLE_RATE = 44100;
constexpr int BANK_VOICES = 32;          ///< voices in a DX7 bank (.syx, 4104 bytes)
constexpr int PACKED_VOICE = 128;        ///< packed voice bytes in a bank
constexpr int VOICE_PARAMS = 155;        ///< unpacked voice parameters

/** Start the engine and its render task; opens the speaker at 44.1 kHz. */
bool start();
/** Stop rendering and release the speaker. */
void stop();
bool running();

void note_on(int midi_note, int velocity);
void note_off(int midi_note);
void all_notes_off();
void controller(int cc, int value);       ///< MIDI CC (1 = mod wheel, 64 = sustain, ...)

/** Load a 32-voice packed bank; returns false if the size is wrong. */
bool load_bank(const uint8_t *packed_bank, size_t len);
/** Select a voice of the loaded bank (0..31). */
void select_voice(int index);
int  voice_count();
/** Name of a voice in the loaded bank (10 chars, trimmed). */
const char *voice_name(int index);
/** Current voice's algorithm (1..32) and feedback (0..7). */
int algorithm();
int feedback();

/** Copy of the current voice, unpacked (155 params), and apply an edited copy live. */
void get_voice(uint8_t out[VOICE_PARAMS]);
void set_voice(const uint8_t in[VOICE_PARAMS]);

/** Relay tap: when on, every rendered stereo block is also queued for relay_read(). */
void set_relay(bool on);
bool relay();
/** Take up to `max` bytes of 16-bit stereo PCM from the tap (single consumer). */
int  relay_read(uint8_t *out, int max);
/** Discard whatever is queued in the tap (on connect, so old audio does not become lag). */
void relay_flush();
/** Mute the Tab5's own speaker (used while the sound goes to Sonos); remembered across restarts of the engine. */
void set_local_mute(bool on);

/** Peak of the last rendered block, 0..32767, for the meter. */
int  last_peak();
/** Diagnostics: blocks rendered, MIDI bytes queued, bytes still unread by the engine. */
void debug_stats(uint32_t &blocks, uint32_t &midi_bytes, int &pending);
int  active_voices();

}
