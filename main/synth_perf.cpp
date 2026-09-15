/**
 * @file synth_perf.cpp
 * @brief Scale lock, diatonic chord pads and the arpeggiator.
 *
 * Model: a set of "held" notes (from keys and the active chord pad) plus the arp mode.
 *  - Arp off: a note sounds from the moment it is held until it is released.
 *  - Arp on: held notes are silent by themselves; an esp_timer steps through them in the
 *    chosen order at the tempo and rate, sounding each for gate% of the step.
 * The engine is driven with MIDI messages only, so this layer never touches audio.
 * All state changes run under one mutex because they come from the LVGL task, the console
 * task and the timer task.
 */
#include "synth_perf.h"
#include "synth_engine.h"
#include "settings.h"

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "perf";

namespace perf {
namespace {

// ------------------------------------------------------------------ scale tables
struct ModeDef { const char *name; uint8_t steps[7]; };
const ModeDef MODES[MODE_COUNT] = {
    { "major",      {0, 2, 4, 5, 7, 9, 11} },
    { "minor",      {0, 2, 3, 5, 7, 8, 10} },
    { "dorian",     {0, 2, 3, 5, 7, 9, 10} },
    { "mixolydian", {0, 2, 4, 5, 7, 9, 10} },
    { "lydian",     {0, 2, 4, 6, 7, 9, 11} },
    { "phrygian",   {0, 1, 3, 5, 7, 8, 10} },
    { "harm minor", {0, 2, 3, 5, 7, 8, 11} },
};
const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char *RATE_NAMES[RATES] = {"1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const float RATE_BEATS[RATES] = {2.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};

constexpr int CHORD_BASE = 48;               ///< chord roots are placed in the octave starting at C3
constexpr int MAX_HELD = 16;
constexpr int VELOCITY = 100;

// ------------------------------------------------------------------ state
SemaphoreHandle_t mtx;
int  s_root = 0; Mode s_mode = MAJOR; bool s_lock = false;
bool s_sevenths = false; int s_voicing = 1; bool s_spread = false; bool s_latch = false;
int  s_pad = -1;                              ///< pad currently sounding (held or latched)
Arp  s_arp = ARP_OFF; int s_rate = 4; int s_gate = 60; int s_tempo = 120;

uint8_t held[MAX_HELD]; int n_held = 0;       ///< notes wanted by keys and pads, in press order
uint8_t chord_notes[4]; int n_chord = 0;      ///< notes contributed by the active pad
uint8_t key_map[128];                         ///< key_map[pressed note] = note actually sounding (after scale lock), 0 = none
uint8_t sounding[MAX_HELD]; int n_sounding = 0;   ///< notes currently sent to the engine (direct mode)

esp_timer_handle_t step_timer, gate_timer;
int arp_pos = -1; int arp_note = -1;          ///< step index and the note the arp is sounding now
void (*change_cb)() = nullptr;

void lock()   { xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(mtx); }
void changed() { if (change_cb) change_cb(); }

// ------------------------------------------------------------------ scale helpers
int scale_degree_note(int degree)             ///< pitch class of scale degree 0..6 (wraps)
{
    return (s_root + MODES[s_mode].steps[((degree % 7) + 7) % 7]) % 12;
}
bool in_scale(int note)
{
    int pc = ((note - s_root) % 12 + 12) % 12;
    for (int i = 0; i < 7; i++) if (MODES[s_mode].steps[i] == pc) return true;
    return false;
}
/** Nearest scale note at or below `note` (a key between two scale tones plays the lower one). */
int snap(int note)
{
    for (int d = 0; d < 12; d++) if (in_scale(note - d)) return note - d;
    return note;
}

// ------------------------------------------------------------------ chord construction
/** Notes of the chord on `degree`, respecting sevenths, voicing and spread. */
int build_chord(int degree, uint8_t out[4])
{
    int n = s_sevenths ? 4 : 3;
    int root_pc = scale_degree_note(degree);
    int notes[4];
    notes[0] = CHORD_BASE + root_pc;
    for (int i = 1; i < n; i++) {
        int pc = scale_degree_note(degree + 2 * i);           // stacked thirds within the scale
        int v = CHORD_BASE + pc;
        while (v < notes[i - 1]) v += 12;                     // keep the stack ascending
        notes[i] = v;
    }
    for (int inv = 1; inv < s_voicing; inv++) {               // inversions: lowest note up an octave
        int low = notes[0];
        for (int i = 0; i < n - 1; i++) notes[i] = notes[i + 1];
        notes[n - 1] = low + 12;
    }
    if (s_spread) {                                           // open voicing: middle voice up, bass added
        if (n == 3) notes[1] += 12;
        for (int i = n; i > 0; i--) notes[i] = notes[i - 1];
        notes[0] = notes[1] - 12;
        n++;
        if (n > 4) n = 4;                                     // sevenths + spread: drop the top note
    }
    for (int i = 0; i < n; i++) out[i] = (uint8_t)notes[i];
    return n;
}

// ------------------------------------------------------------------ held-note bookkeeping
void hold(int note)
{
    for (int i = 0; i < n_held; i++) if (held[i] == note) return;
    if (n_held < MAX_HELD) held[n_held++] = (uint8_t)note;
}
void unhold(int note)
{
    for (int i = 0; i < n_held; i++) if (held[i] == note) { for (int j = i; j < n_held - 1; j++) held[j] = held[j + 1]; n_held--; return; }
}
bool is_held(int note) { for (int i = 0; i < n_held; i++) if (held[i] == note) return true; return false; }

/** Direct mode: make the engine's sounding set equal the held set. */
void sync_direct()
{
    for (int i = 0; i < n_sounding; i++) if (!is_held(sounding[i])) synth::note_off(sounding[i]);
    for (int i = 0; i < n_held; i++) {
        bool on = false;
        for (int j = 0; j < n_sounding; j++) if (sounding[j] == held[i]) on = true;
        if (!on) synth::note_on(held[i], VELOCITY);
    }
    memcpy(sounding, held, n_held); n_sounding = n_held;
}

void silence_direct()
{
    for (int i = 0; i < n_sounding; i++) synth::note_off(sounding[i]);
    n_sounding = 0;
}

// ------------------------------------------------------------------ arpeggiator
int64_t step_us() { return (int64_t)(60000000.0f / s_tempo * RATE_BEATS[s_rate]); }

void arp_note_off()
{
    if (arp_note >= 0) { synth::note_off(arp_note); arp_note = -1; }
}

/** One arpeggiator step: pick the next held note in the chosen order and sound it. Caller holds the lock. */
void arp_advance()
{
    arp_note_off();
    if (s_arp != ARP_OFF && n_held > 0) {
        uint8_t order[MAX_HELD]; memcpy(order, held, n_held);
        for (int i = 1; i < n_held; i++)                      // sort ascending (insertion sort, tiny n)
            for (int j = i; j > 0 && order[j - 1] > order[j]; j--) { uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
        switch (s_arp) {
        case ARP_UP:     arp_pos = (arp_pos + 1) % n_held; break;
        case ARP_DOWN:   arp_pos = arp_pos <= 0 ? n_held - 1 : (arp_pos - 1) % n_held; break;
        case ARP_RANDOM: arp_pos = (int)(esp_random() % n_held); break;
        default: break;
        }
        if (arp_pos >= n_held) arp_pos = 0;
        arp_note = order[arp_pos];
        synth::note_on(arp_note, VELOCITY);
        int64_t step = step_us();
        esp_timer_start_once(gate_timer, step * s_gate / 100);
        esp_timer_start_once(step_timer, step);
    }
}

void arp_step(void *) { lock(); arp_advance(); unlock(); changed(); }

void arp_gate(void *) { lock(); arp_note_off(); unlock(); changed(); }

/** (Re)start the step chain if the arp is on and something is held; stop it otherwise. */
void arp_refresh()
{
    esp_timer_stop(step_timer);
    esp_timer_stop(gate_timer);
    arp_note_off();
    if (s_arp != ARP_OFF && n_held > 0) arp_advance();
}

/** Apply a change to the held set in whichever mode is active. */
void held_changed()
{
    if (s_arp == ARP_OFF) sync_direct();
    else if (n_held == 0 || !esp_timer_is_active(step_timer)) arp_refresh();
}

/** Rebuild the active pad's chord after a scale, voicing or spread change. */
void retrigger_pad()
{
    if (s_pad < 0) return;
    for (int i = 0; i < n_chord; i++) unhold(chord_notes[i]);
    n_chord = build_chord(s_pad, chord_notes);
    for (int i = 0; i < n_chord; i++) hold(chord_notes[i]);
    held_changed();
}

void persist()
{
    settings::set_int("pf_root", s_root); settings::set_int("pf_mode", s_mode); settings::set_int("pf_lock", s_lock);
    settings::set_int("pf_7th", s_sevenths); settings::set_int("pf_voic", s_voicing); settings::set_int("pf_sprd", s_spread);
    settings::set_int("pf_latch", s_latch); settings::set_int("pf_arp", s_arp); settings::set_int("pf_rate", s_rate);
    settings::set_int("pf_gate", s_gate); settings::set_int("pf_tempo", s_tempo);
}

} // namespace

// ------------------------------------------------------------------ public: scale
void set_root(int pc) { lock(); s_root = ((pc % 12) + 12) % 12; retrigger_pad(); unlock(); changed(); }
void set_mode(Mode m) { lock(); s_mode = (Mode)(((int)m % MODE_COUNT + MODE_COUNT) % MODE_COUNT); retrigger_pad(); unlock(); changed(); }
void set_scale_lock(bool on) { lock(); s_lock = on; unlock(); }
int  root() { return s_root; }
Mode mode() { return s_mode; }
bool scale_lock() { return s_lock; }
const char *mode_name(Mode m) { return MODES[((int)m % MODE_COUNT + MODE_COUNT) % MODE_COUNT].name; }
const char *note_name(int pc) { return NOTE_NAMES[((pc % 12) + 12) % 12]; }
const char *scale_name()
{
    static char t[24];
    snprintf(t, sizeof t, "%s %s", NOTE_NAMES[s_root], MODES[s_mode].name);
    return t;
}

// ------------------------------------------------------------------ public: chords
void chord_label(int degree, char numeral[8], char symbol[12])
{
    static const char *UPPER[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    static const char *lower[7] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
    const int r = scale_degree_note(degree);
    const int third = ((scale_degree_note(degree + 2) - r) % 12 + 12) % 12;
    const int fifth = ((scale_degree_note(degree + 4) - r) % 12 + 12) % 12;
    const int seventh = ((scale_degree_note(degree + 6) - r) % 12 + 12) % 12;
    const bool minor = third == 3, dim = minor && fifth == 6, aug = !minor && fifth == 8;
    const char *ext = "";
    if (s_sevenths) {
        if (dim)        ext = seventh == 9 ? "7" : "7b5";       // diminished seventh / half-diminished
        else if (minor) ext = seventh == 11 ? "(maj7)" : "7";
        else            ext = seventh == 11 ? "maj7" : "7";
    }
    const char *quality = dim ? (s_sevenths && seventh == 10 ? "m" : "°") : aug ? "+" : minor ? "m" : "";
    snprintf(numeral, 8, "%s%s", (minor || dim) ? lower[degree] : UPPER[degree], dim ? "°" : aug ? "+" : "");
    snprintf(symbol, 12, "%s%s%s", NOTE_NAMES[r], quality, ext);
}
void set_sevenths(bool on) { lock(); s_sevenths = on; retrigger_pad(); unlock(); changed(); }
bool sevenths() { return s_sevenths; }
void set_voicing(int v) { lock(); s_voicing = v < 1 ? 1 : v > 3 ? 3 : v; retrigger_pad(); unlock(); changed(); }
int  voicing() { return s_voicing; }
void set_spread(bool on) { lock(); s_spread = on; retrigger_pad(); unlock(); changed(); }
bool spread() { return s_spread; }
void set_latch(bool on) { lock(); s_latch = on; if (!on && s_pad >= 0) pad_up(s_pad); unlock(); changed(); }
bool latch() { return s_latch; }
int  active_pad() { return s_pad; }

void pad_down(int degree)
{
    if (degree < 0 || degree >= PADS) return;
    lock();
    bool same = s_pad == degree;
    if (s_pad >= 0) {                                         // one pad at a time
        for (int i = 0; i < n_chord; i++) unhold(chord_notes[i]);
        n_chord = 0; s_pad = -1;
    }
    if (!(s_latch && same)) {                                 // latched pad tapped again = release it
        n_chord = build_chord(degree, chord_notes);
        for (int i = 0; i < n_chord; i++) hold(chord_notes[i]);
        s_pad = degree;
    }
    held_changed();
    unlock();
    changed();
}

void pad_up(int degree)
{
    lock();
    if (!s_latch && s_pad == degree) {
        for (int i = 0; i < n_chord; i++) unhold(chord_notes[i]);
        n_chord = 0; s_pad = -1;
        held_changed();
    }
    unlock();
    changed();
}

// ------------------------------------------------------------------ public: keys
void key_down(int note)
{
    if (note < 0 || note > 127) return;
    lock();
    int play = s_lock ? snap(note) : note;
    key_map[note] = (uint8_t)play;
    hold(play);
    held_changed();
    unlock();
    changed();
}

void key_up(int note)
{
    if (note < 0 || note > 127) return;
    lock();
    int play = key_map[note] ? key_map[note] : note;
    key_map[note] = 0;
    bool still_wanted = false;                                // another key may map to the same note
    for (int n = 0; n < 128; n++) if (key_map[n] == play) still_wanted = true;
    for (int i = 0; i < n_chord; i++) if (chord_notes[i] == play) still_wanted = true;
    if (!still_wanted) unhold(play);
    held_changed();
    unlock();
    changed();
}

// ------------------------------------------------------------------ public: arpeggiator
void set_arp(Arp a)
{
    lock();
    if (a != s_arp) {
        s_arp = a;
        if (a == ARP_OFF) { esp_timer_stop(step_timer); esp_timer_stop(gate_timer); arp_note_off(); sync_direct(); }
        else { silence_direct(); arp_pos = -1; arp_refresh(); }
    }
    unlock();
    changed();
}
Arp  arp() { return s_arp; }
void set_rate(int i) { lock(); s_rate = i < 0 ? 0 : i >= RATES ? RATES - 1 : i; unlock(); }
int  rate() { return s_rate; }
const char *rate_name(int i) { return RATE_NAMES[i < 0 ? 0 : i >= RATES ? RATES - 1 : i]; }
void set_gate(int p) { lock(); s_gate = p < 10 ? 10 : p > 100 ? 100 : p; unlock(); }
int  gate() { return s_gate; }
void set_tempo(int bpm) { lock(); s_tempo = bpm < 40 ? 40 : bpm > 240 ? 240 : bpm; unlock(); }

void save() { lock(); persist(); unlock(); }
int  tempo() { return s_tempo; }

void all_off()
{
    lock();
    esp_timer_stop(step_timer); esp_timer_stop(gate_timer);
    arp_note_off();
    silence_direct();
    n_held = 0; n_chord = 0; s_pad = -1; memset(key_map, 0, sizeof key_map);
    synth::all_notes_off();
    unlock();
    changed();
}

uint32_t sounding_mask(int base, int span)
{
    uint32_t m = 0;
    lock();
    if (s_arp == ARP_OFF) { for (int i = 0; i < n_sounding; i++) { int b = sounding[i] - base; if (b >= 0 && b < span) m |= 1u << b; } }
    else if (arp_note >= 0) { int b = arp_note - base; if (b >= 0 && b < span) m |= 1u << b; }
    for (int i = 0; i < n_chord; i++) { int b = chord_notes[i] - base; if (b >= 0 && b < span) m |= 1u << b; }
    unlock();
    return m;
}

void on_change(void (*cb)()) { change_cb = cb; }

void init()
{
    if (mtx) return;
    mtx = xSemaphoreCreateRecursiveMutex();
    s_root = settings::get_int("pf_root", 0); s_mode = (Mode)settings::get_int("pf_mode", MAJOR);
    s_lock = settings::get_int("pf_lock", 0); s_sevenths = settings::get_int("pf_7th", 0);
    s_voicing = settings::get_int("pf_voic", 1); s_spread = settings::get_int("pf_sprd", 0);
    s_latch = settings::get_int("pf_latch", 0); s_arp = (Arp)settings::get_int("pf_arp", ARP_OFF);
    s_rate = settings::get_int("pf_rate", 4); s_gate = settings::get_int("pf_gate", 60); s_tempo = settings::get_int("pf_tempo", 120);
    esp_timer_create_args_t a = {}; a.callback = arp_step; a.name = "arp_step"; esp_timer_create(&a, &step_timer);
    esp_timer_create_args_t g = {}; g.callback = arp_gate; g.name = "arp_gate"; esp_timer_create(&g, &gate_timer);
    ESP_LOGI(TAG, "scale %s, arp %d, %d bpm", scale_name(), s_arp, s_tempo);
}

}
