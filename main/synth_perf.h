/**
 * @file synth_perf.h
 * @brief Performance layer between the keys/pads and the FM engine: scale lock, chords, arpeggiator.
 *
 * Everything the player touches goes through here. Keys and chord pads produce "held"
 * notes; with the arpeggiator off they sound directly, with it on they are cycled by a
 * timer. The scale decides which notes the keyboard may play (when LOCK is on) and which
 * chords the eight pads carry (the diatonic triads or sevenths of the current key).
 */
#pragma once
#include <cstdint>

namespace perf {

// ---- scale ---------------------------------------------------------------------------
enum Mode { MAJOR, MINOR, DORIAN, MIXOLYDIAN, LYDIAN, PHRYGIAN, HARMONIC_MINOR, MODE_COUNT };

void set_root(int pitch_class);              ///< 0 = C .. 11 = B
void set_mode(Mode m);
void set_scale_lock(bool on);                ///< snap keyboard notes onto the scale
int  root();
Mode mode();
bool scale_lock();
const char *mode_name(Mode m);               ///< "major", "minor", "dorian", ...
const char *note_name(int pitch_class);      ///< "C", "C#", ...
/** Scale text for the readouts, e.g. "C major". */
const char *scale_name();

// ---- chords ----------------------------------------------------------------------------
constexpr int PADS = 7;                      ///< one pad per scale degree

/** Label for pad `degree` (0..6): roman numeral and chord symbol, e.g. "ii", "Dm7". */
void chord_label(int degree, char numeral[8], char symbol[12]);
void set_sevenths(bool on);                  ///< pads play four-note sevenths instead of triads
bool sevenths();
void set_voicing(int v);                     ///< 1 root position, 2 first inversion, 3 second inversion
int  voicing();
void set_spread(bool on);                    ///< open voicing: bass note an octave down, middle voice up
bool spread();
void set_latch(bool on);                     ///< a tapped pad keeps sounding until the next tap
bool latch();
void pad_down(int degree);
void pad_up(int degree);
int  active_pad();                           ///< latched or held pad, or -1

// ---- keys ----------------------------------------------------------------------------------
void key_down(int midi_note);
void key_up(int midi_note);

// ---- arpeggiator ----------------------------------------------------------------------------
enum Arp { ARP_OFF, ARP_UP, ARP_DOWN, ARP_RANDOM };
constexpr int RATES = 7;                     ///< see rate_name()

void set_arp(Arp a);
Arp  arp();
void set_rate(int index);                    ///< 0..RATES-1
int  rate();
const char *rate_name(int index);            ///< "1/4", "1/8", "1/8T", ...
void set_gate(int percent);                  ///< 10..100
int  gate();
void set_tempo(int bpm);                     ///< 40..240
int  tempo();

/** Release everything: held notes, latched pad, arpeggiator step. */
void all_off();

/** Bit mask (bit n = MIDI note n - base) of notes currently sounding or latched, for key highlights. */
uint32_t sounding_mask(int base_note, int span);

/** Callback when the set of sounding notes changes (LVGL-safe: called from the arp timer or UI). */
void on_change(void (*cb)());

/** Write the scale, chord and arpeggiator settings to NVS (call on release, not per drag). */
void save();

/** Restore persisted settings and create the arpeggiator timer. Call once. */
void init();

}
