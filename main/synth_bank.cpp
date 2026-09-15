/**
 * @file synth_bank.cpp
 * @brief Factory voices for the FM synth, written as parameter tables.
 *
 * Every voice is a VoiceSpec: an algorithm, feedback, six operators (operator 1
 * first, the way the DX7 panel numbers them) and the LFO. pack() turns a spec into
 * the 128-byte DX7 bulk voice layout the engine loads over its sysex path. Keeping
 * the voices as tables rather than hex dumps makes them easy to tweak and review.
 *
 * DX7 conventions used here:
 *  - Frequency ratio = coarse (0 means 0.5) x (1 + fine/100); detune 0..14, 7 = centre.
 *  - Envelope rates R1..R4 (99 = instant) and levels L1..L4 (99 = full). L4 is the
 *    level after release, so sustaining voices have L3 high and L4 = 0.
 *  - Output level 0..99. Modulator level sets brightness; carrier level sets loudness.
 *  - KVS = velocity sensitivity 0..7, RS = rate scaling (higher notes decay faster).
 */
#include "synth_bank.h"

#include <cstring>

namespace synth_bank {
namespace {

/** One operator. Keyboard level scaling is left flat (break point C3, no depth). */
struct OpSpec {
    int coarse, fine, detune;     ///< frequency ratio and detune
    int ol;                       ///< output level 0..99
    int r1, r2, r3, r4;           ///< envelope rates
    int l1, l2, l3, l4;           ///< envelope levels
    int kvs, rs, ams;             ///< velocity sensitivity, rate scaling, amp-mod sensitivity
};

struct VoiceSpec {
    const char *name;             ///< up to 10 characters, DX7 style
    int alg, fb;                  ///< algorithm 1..32, feedback 0..7
    OpSpec op[6];                 ///< operator 1 first
    int lfs, lfd, lpmd, lamd;     ///< LFO speed, delay, pitch depth, amplitude depth
    int lfw, lpms;                ///< LFO wave (0 tri, 1 saw down, 2 saw up, 3 square, 4 sine, 5 s/h), pitch mod sens
    int transpose;                ///< 24 = C3 (no transpose); 12 = an octave down
};

// Envelope shorthands: rates then levels.
#define PERC        95, 28, 20, 55,   99, 88, 0, 0     ///< piano-like: instant, long decay, dies while held
#define PERC_FAST   99, 55, 40, 70,   99, 65, 0, 0     ///< plucks and clavs: short decay
#define BELL        99, 18, 14, 35,   99, 80, 0, 0     ///< very long decay
#define SUS         80, 60, 50, 55,   99, 95, 90, 0    ///< sustaining while held, medium release
#define SLOW        42, 45, 40, 40,   99, 96, 94, 0    ///< strings and pads: soft attack
#define ORGAN       99, 99, 99, 90,   99, 99, 99, 0    ///< switch on, switch off
#define BRASS       62, 68, 55, 55,   99, 84, 80, 0    ///< slight blossom at the attack
#define REED        72, 60, 50, 60,   99, 90, 88, 0    ///< woodwind-style attack

// Operator shorthands: coarse, fine, detune, level, envelope (a shorthand above or eight
// numbers), kvs, rs. CAR and MOD are the same data; the names document the routing.
#define CAR(c, f, d, ol, ...)  { c, f, d, ol, __VA_ARGS__, 0 }
#define MOD(c, f, d, ol, ...)  { c, f, d, ol, __VA_ARGS__, 0 }
#define AMP(c, f, d, ol, ...)  { c, f, d, ol, __VA_ARGS__, 2 }   ///< carrier that follows LFO amplitude modulation
#define OFF                             { 1, 0, 7, 0, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0 }
#define NO_LFO      35, 0, 0, 0, 0, 3
#define VIBRATO     34, 45, 10, 0, 4, 3         ///< delayed sine vibrato
#define TREMOLO     40, 0, 0, 38, 0, 3          ///< triangle amplitude modulation (operators marked AMP respond)

/*
 * Algorithm cheat sheet (carriers listed first):
 *   1  carriers 1,3        2->1, 6->5->4->3, feedback 6
 *   2  carriers 1,3        2->1 (feedback 2), 6->5->4->3
 *   5  carriers 1,3,5      2->1, 4->3, 6->5, feedback 6
 *   7  carriers 1,3        2->1, 4->3 and 5->3, 6->5, feedback 6
 *   8  carriers 1,3        2->1, 4->3 (feedback 4), 5->3, 6->5
 *  16  carrier 1           2->1, 3->1, 5->1, 4->3, 6->5, feedback 6
 *  18  carrier 1           2->1, 3->1 (feedback 3), 4->3, 5->4, 6->5
 *  22  carriers 1,3,4,5    2->1, 6->3,4,5, feedback 6
 *  32  all carriers        feedback 6
 */
const VoiceSpec VOICES[32] = {
    // ---- keyboards -----------------------------------------------------------------
    { "E.PIANO 1 ", 5, 6, {
        CAR(1, 0, 7, 99, PERC, 3, 2),      MOD(14, 0, 7, 58, 95, 65, 35, 60, 99, 0, 0, 0, 5, 3),   // tine
        CAR(1, 0, 7, 99, PERC, 3, 2),      MOD(1, 0, 7, 78, 95, 30, 20, 55, 99, 80, 0, 0, 4, 2),
        CAR(1, 0, 10, 92, PERC, 3, 2),     MOD(1, 0, 7, 62, 95, 30, 20, 55, 99, 72, 0, 0, 4, 2) },
      NO_LFO, 24 },
    { "TINE PIANO", 5, 5, {
        CAR(1, 0, 7, 99, PERC, 3, 2),      MOD(7, 0, 7, 66, 99, 70, 40, 60, 99, 0, 0, 0, 6, 3),
        CAR(1, 0, 4, 96, PERC, 3, 2),      MOD(1, 0, 7, 80, 95, 28, 20, 55, 99, 82, 0, 0, 4, 2),
        CAR(2, 0, 7, 70, PERC, 3, 2),      MOD(1, 0, 7, 55, 95, 30, 20, 55, 99, 70, 0, 0, 4, 2) },
      TREMOLO, 24 },
    { "GRAND PNO ", 5, 4, {
        CAR(1, 0, 7, 99, PERC, 4, 2),      MOD(1, 0, 7, 84, 96, 32, 22, 55, 99, 78, 0, 0, 5, 3),
        CAR(2, 0, 9, 84, PERC, 4, 2),      MOD(3, 0, 7, 62, 99, 45, 30, 60, 99, 60, 0, 0, 5, 3),
        CAR(1, 0, 5, 90, PERC, 4, 2),      MOD(6, 0, 7, 48, 99, 60, 40, 60, 99, 40, 0, 0, 5, 3) },
      NO_LFO, 24 },
    { "CLAVINET  ", 5, 7, {
        CAR(1, 0, 7, 99, PERC_FAST, 5, 3), MOD(3, 0, 7, 86, 99, 62, 45, 75, 99, 55, 0, 0, 6, 3),
        CAR(2, 0, 7, 82, PERC_FAST, 5, 3), MOD(4, 0, 7, 78, 99, 65, 45, 75, 99, 50, 0, 0, 6, 3),
        CAR(1, 0, 9, 60, PERC_FAST, 5, 3), MOD(1, 0, 7, 70, 99, 60, 45, 75, 99, 50, 0, 0, 6, 3) },
      NO_LFO, 24 },
    { "HARPSICHRD", 5, 6, {
        CAR(1, 0, 7, 99, 99, 50, 38, 99, 99, 70, 0, 0, 2, 3),  MOD(3, 0, 7, 88, 99, 55, 40, 99, 99, 60, 0, 0, 2, 3),
        CAR(2, 0, 7, 78, 99, 50, 38, 99, 99, 70, 0, 0, 2, 3),  MOD(5, 0, 7, 72, 99, 60, 45, 99, 99, 50, 0, 0, 2, 3),
        CAR(4, 0, 7, 50, 99, 50, 38, 99, 99, 60, 0, 0, 2, 3),  MOD(1, 0, 7, 60, 99, 55, 40, 99, 99, 50, 0, 0, 2, 3) },
      NO_LFO, 24 },

    // ---- mallets and bells ---------------------------------------------------------
    { "VIBES     ", 5, 0, {
        AMP(1, 0, 7, 99, BELL, 3, 3),      MOD(4, 0, 7, 70, 99, 40, 30, 60, 99, 55, 0, 0, 5, 4),
        AMP(1, 0, 8, 80, BELL, 3, 3),      MOD(1, 0, 7, 50, 99, 30, 25, 60, 99, 50, 0, 0, 4, 3),
        OFF,                               OFF },
      TREMOLO, 24 },
    { "MARIMBA   ", 5, 0, {
        CAR(1, 0, 7, 99, 99, 62, 45, 80, 99, 45, 0, 0, 4, 4),  MOD(4, 0, 7, 80, 99, 80, 60, 80, 99, 30, 0, 0, 5, 4),
        CAR(1, 0, 7, 60, 99, 70, 50, 80, 99, 30, 0, 0, 4, 4),  MOD(10, 0, 7, 55, 99, 90, 70, 80, 99, 0, 0, 0, 6, 4),
        OFF,                               OFF },
      NO_LFO, 24 },
    { "TUBULAR   ", 5, 3, {
        CAR(1, 0, 7, 99, BELL, 2, 2),      MOD(3, 50, 7, 74, 99, 22, 18, 40, 99, 78, 0, 0, 3, 2),
        CAR(1, 41, 7, 82, BELL, 2, 2),     MOD(2, 0, 7, 60, 99, 25, 20, 40, 99, 70, 0, 0, 3, 2),
        CAR(2, 0, 7, 50, BELL, 2, 2),      MOD(7, 0, 7, 40, 99, 35, 25, 45, 99, 40, 0, 0, 3, 2) },
      NO_LFO, 24 },
    { "GLASS BELL", 5, 4, {
        CAR(2, 0, 7, 99, BELL, 2, 2),      MOD(7, 0, 7, 58, 99, 20, 16, 40, 99, 72, 0, 0, 4, 2),
        CAR(3, 0, 9, 74, BELL, 2, 2),      MOD(11, 0, 7, 46, 99, 26, 20, 40, 99, 60, 0, 0, 4, 2),
        CAR(1, 0, 7, 70, BELL, 2, 2),      MOD(1, 0, 7, 44, 99, 20, 16, 40, 99, 65, 0, 0, 3, 2) },
      NO_LFO, 24 },
    { "MUSIC BOX ", 5, 2, {
        CAR(2, 0, 7, 99, 99, 40, 30, 60, 99, 60, 0, 0, 3, 3),  MOD(5, 0, 7, 70, 99, 60, 40, 60, 99, 40, 0, 0, 4, 3),
        CAR(4, 0, 7, 64, 99, 45, 30, 60, 99, 50, 0, 0, 3, 3),  MOD(8, 0, 7, 50, 99, 65, 45, 60, 99, 30, 0, 0, 4, 3),
        OFF,                               OFF },
      NO_LFO, 36 },

    // ---- basses ----------------------------------------------------------------------
    { "SYN BASS  ", 1, 5, {
        CAR(0, 0, 7, 99, 99, 42, 30, 65, 99, 82, 60, 0, 3, 2), MOD(1, 0, 7, 82, 99, 58, 35, 65, 99, 55, 0, 0, 5, 3),
        CAR(1, 0, 7, 92, 99, 45, 30, 65, 99, 80, 55, 0, 3, 2), MOD(1, 0, 7, 70, 99, 60, 35, 65, 99, 50, 0, 0, 5, 3),
        OFF,                               OFF },
      NO_LFO, 12 },
    { "SOLID BASS", 16, 6, {
        CAR(1, 0, 7, 99, 99, 45, 32, 68, 99, 85, 62, 0, 4, 2), MOD(1, 0, 7, 84, 99, 55, 35, 68, 99, 62, 0, 0, 5, 3),
        MOD(2, 0, 7, 56, 99, 65, 40, 68, 99, 45, 0, 0, 5, 3),  MOD(1, 0, 7, 40, 99, 70, 45, 68, 99, 40, 0, 0, 4, 3),
        MOD(0, 0, 7, 52, 99, 50, 35, 68, 99, 70, 40, 0, 3, 2), MOD(1, 0, 7, 48, 99, 70, 45, 68, 99, 40, 0, 0, 4, 3) },
      NO_LFO, 12 },
    { "SLAP BASS ", 5, 0, {
        CAR(1, 0, 7, 99, 99, 52, 35, 72, 99, 75, 45, 0, 4, 3), MOD(3, 0, 7, 92, 99, 88, 70, 80, 99, 20, 0, 0, 7, 4),
        CAR(0, 0, 7, 95, 99, 48, 32, 72, 99, 78, 50, 0, 3, 2), MOD(1, 0, 7, 72, 99, 60, 40, 72, 99, 52, 0, 0, 5, 3),
        OFF,                               OFF },
      NO_LFO, 12 },
    { "FRETLESS  ", 2, 5, {
        CAR(1, 0, 7, 99, 84, 50, 38, 62, 99, 88, 70, 0, 3, 2), MOD(1, 0, 7, 72, 82, 45, 30, 62, 99, 70, 30, 0, 4, 2),
        CAR(0, 0, 7, 86, 84, 50, 38, 62, 99, 88, 72, 0, 3, 2), MOD(1, 0, 7, 40, 84, 45, 30, 62, 99, 60, 20, 0, 4, 2),
        MOD(2, 0, 7, 30, 84, 45, 30, 62, 99, 50, 10, 0, 4, 2), OFF },
      VIBRATO, 12 },

    // ---- brass and strings ---------------------------------------------------------
    { "BRASS 1   ", 18, 6, {
        CAR(1, 0, 7, 99, BRASS, 3, 1),     MOD(1, 0, 7, 78, BRASS, 4, 2),
        MOD(1, 0, 7, 72, 58, 62, 50, 55, 99, 80, 76, 0, 4, 2), MOD(1, 0, 7, 46, BRASS, 3, 2),
        OFF,                               OFF },
      VIBRATO, 24 },
    { "SYN BRASS ", 22, 6, {
        CAR(1, 0, 4, 99, BRASS, 3, 1),     MOD(1, 0, 7, 76, BRASS, 4, 2),
        CAR(1, 0, 10, 92, BRASS, 3, 1),    CAR(1, 0, 7, 92, BRASS, 3, 1),
        CAR(2, 0, 7, 70, BRASS, 3, 1),     MOD(1, 0, 7, 68, 60, 64, 50, 55, 99, 82, 78, 0, 4, 2) },
      34, 30, 5, 0, 4, 3, 24 },
    { "STRINGS   ", 2, 4, {
        CAR(1, 0, 5, 99, SLOW, 2, 1),      MOD(1, 0, 7, 62, SLOW, 3, 1),
        CAR(1, 0, 9, 96, SLOW, 2, 1),      MOD(1, 0, 7, 55, SLOW, 3, 1),
        MOD(3, 0, 7, 40, SLOW, 3, 1),      MOD(1, 0, 7, 30, SLOW, 3, 1) },
      34, 40, 12, 0, 4, 3, 24 },
    { "WARM PAD  ", 2, 3, {
        CAR(1, 0, 6, 99, 32, 40, 35, 35, 99, 96, 94, 0, 1, 1), MOD(1, 0, 7, 52, 30, 40, 35, 35, 99, 90, 88, 0, 2, 1),
        CAR(0, 0, 8, 90, 30, 40, 35, 35, 99, 96, 94, 0, 1, 1), MOD(1, 0, 7, 46, 30, 40, 35, 35, 99, 90, 88, 0, 2, 1),
        MOD(2, 0, 7, 32, 30, 40, 35, 35, 99, 80, 78, 0, 2, 1), MOD(1, 0, 7, 22, 30, 40, 35, 35, 99, 80, 78, 0, 2, 1) },
      25, 60, 6, 0, 4, 3, 24 },
    { "GLASS PAD ", 5, 2, {
        CAR(1, 0, 7, 99, SLOW, 2, 1),      MOD(3, 0, 7, 52, SLOW, 3, 1),
        CAR(2, 0, 9, 84, SLOW, 2, 1),      MOD(2, 0, 7, 48, SLOW, 3, 1),
        CAR(1, 0, 4, 80, SLOW, 2, 1),      MOD(5, 0, 7, 40, SLOW, 3, 1) },
      28, 50, 5, 0, 4, 3, 24 },
    { "CHOIR     ", 7, 3, {
        CAR(1, 0, 7, 99, SLOW, 2, 1),      MOD(1, 0, 7, 55, SLOW, 3, 1),
        CAR(1, 0, 10, 94, SLOW, 2, 1),     MOD(2, 0, 7, 45, SLOW, 3, 1),
        MOD(3, 0, 7, 40, SLOW, 3, 1),      MOD(1, 0, 7, 35, SLOW, 3, 1) },
      30, 55, 8, 0, 4, 3, 24 },

    // ---- organs ----------------------------------------------------------------------
    { "PIPE ORGAN", 32, 0, {
        CAR(1, 0, 7, 99, ORGAN, 0, 0),     CAR(2, 0, 7, 85, ORGAN, 0, 0),
        CAR(3, 0, 7, 72, ORGAN, 0, 0),     CAR(4, 0, 7, 68, ORGAN, 0, 0),
        CAR(0, 0, 7, 90, ORGAN, 0, 0),     CAR(6, 0, 7, 58, ORGAN, 0, 0) },
      NO_LFO, 24 },
    { "JAZZ ORGAN", 32, 0, {
        AMP(1, 0, 7, 99, ORGAN, 1, 0),     AMP(2, 0, 7, 80, ORGAN, 1, 0),
        AMP(3, 0, 7, 68, ORGAN, 1, 0),     AMP(4, 0, 7, 58, ORGAN, 1, 0),
        AMP(0, 0, 7, 84, ORGAN, 1, 0),     AMP(8, 0, 7, 40, ORGAN, 1, 0) },
      TREMOLO, 24 },
    { "PERC ORGAN", 22, 5, {
        CAR(1, 0, 7, 99, ORGAN, 1, 0),     MOD(3, 0, 7, 80, 99, 75, 55, 90, 99, 10, 0, 0, 3, 3),   // key click
        CAR(2, 0, 7, 82, ORGAN, 1, 0),     CAR(3, 0, 7, 60, ORGAN, 1, 0),
        CAR(0, 0, 7, 86, ORGAN, 1, 0),     MOD(1, 0, 7, 40, ORGAN, 2, 0) },
      NO_LFO, 24 },

    // ---- winds -----------------------------------------------------------------------
    { "FLUTE     ", 8, 7, {
        CAR(1, 0, 7, 99, 62, 55, 50, 58, 99, 92, 90, 0, 2, 1), MOD(1, 0, 7, 44, 62, 55, 50, 58, 99, 90, 88, 0, 3, 1),
        CAR(2, 0, 7, 58, 62, 55, 50, 58, 99, 90, 88, 0, 2, 1), MOD(1, 0, 7, 28, 90, 60, 50, 58, 99, 60, 55, 0, 3, 1),  // breath, via feedback
        OFF,                               OFF },
      34, 50, 10, 0, 4, 3, 24 },
    { "CLARINET  ", 2, 0, {
        CAR(1, 0, 7, 99, REED, 2, 1),      MOD(2, 0, 7, 70, REED, 3, 1),          // 2:1 gives the odd harmonics
        CAR(1, 0, 7, 30, REED, 2, 1),      MOD(3, 0, 7, 40, REED, 3, 1),
        OFF,                               OFF },
      34, 40, 6, 0, 4, 3, 24 },
    { "OBOE      ", 2, 3, {
        CAR(1, 0, 7, 99, REED, 2, 1),      MOD(1, 0, 7, 78, REED, 3, 1),
        CAR(2, 0, 7, 46, REED, 2, 1),      MOD(2, 0, 7, 52, REED, 3, 1),
        MOD(1, 0, 7, 30, REED, 3, 1),      OFF },
      34, 40, 8, 0, 4, 3, 24 },
    { "HARMONICA ", 5, 5, {
        AMP(1, 0, 7, 99, SUS, 2, 1),       MOD(1, 0, 7, 80, SUS, 3, 1),
        AMP(2, 0, 7, 62, SUS, 2, 1),       MOD(3, 0, 7, 50, SUS, 3, 1),
        OFF,                               MOD(1, 0, 7, 40, SUS, 3, 1) },
      40, 30, 4, 25, 0, 3, 24 },

    // ---- leads and plucks ------------------------------------------------------------
    { "SYN LEAD  ", 16, 7, {
        CAR(1, 0, 7, 99, SUS, 3, 1),       MOD(1, 0, 7, 88, SUS, 4, 1),
        MOD(2, 0, 7, 60, SUS, 4, 1),       MOD(1, 0, 7, 40, SUS, 3, 1),
        MOD(0, 0, 7, 40, SUS, 3, 1),       MOD(1, 0, 7, 50, SUS, 3, 1) },
      34, 60, 15, 0, 4, 3, 24 },
    { "WAH LEAD  ", 16, 5, {
        CAR(1, 0, 7, 99, SUS, 3, 1),       MOD(1, 0, 7, 95, 45, 40, 30, 60, 99, 70, 40, 0, 5, 1),   // level sweep = wah
        MOD(2, 0, 7, 40, SUS, 4, 1),       OFF,
        MOD(0, 0, 7, 46, SUS, 3, 1),       MOD(1, 0, 7, 44, SUS, 3, 1) },
      NO_LFO, 24 },
    { "NYLON GTR ", 5, 3, {
        CAR(1, 0, 7, 99, 99, 42, 32, 70, 99, 78, 0, 0, 4, 3),  MOD(1, 0, 7, 90, 99, 60, 42, 70, 99, 55, 0, 0, 5, 3),
        CAR(2, 0, 7, 62, 99, 45, 35, 70, 99, 70, 0, 0, 4, 3),  MOD(3, 0, 7, 58, 99, 70, 50, 70, 99, 40, 0, 0, 5, 3),
        CAR(1, 0, 9, 40, 99, 42, 32, 70, 99, 78, 0, 0, 4, 3),  MOD(5, 0, 7, 40, 99, 75, 55, 70, 99, 30, 0, 0, 5, 3) },
      NO_LFO, 24 },
    { "KOTO      ", 5, 4, {
        CAR(1, 0, 7, 99, 99, 50, 32, 70, 99, 72, 0, 0, 4, 3),  MOD(2, 0, 7, 82, 99, 72, 50, 75, 99, 45, 0, 0, 6, 3),
        CAR(3, 0, 7, 56, 99, 55, 35, 70, 99, 60, 0, 0, 4, 3),  MOD(1, 0, 7, 62, 99, 75, 55, 75, 99, 40, 0, 0, 6, 3),
        OFF,                               OFF },
      NO_LFO, 24 },
    { "STEEL DRUM", 5, 2, {
        CAR(1, 0, 7, 99, 99, 32, 24, 50, 99, 80, 0, 0, 3, 3),  MOD(2, 30, 7, 70, 99, 45, 30, 55, 99, 60, 0, 0, 5, 3),
        CAR(3, 6, 9, 68, 99, 35, 26, 50, 99, 75, 0, 0, 3, 3),  MOD(4, 0, 7, 56, 99, 50, 35, 55, 99, 55, 0, 0, 5, 3),
        CAR(1, 0, 5, 50, 99, 32, 24, 50, 99, 80, 0, 0, 3, 3),  MOD(7, 0, 7, 40, 99, 55, 40, 55, 99, 45, 0, 0, 5, 3) },
      NO_LFO, 24 },
};

/** Pack one spec into the 128-byte DX7 bulk voice layout (operator 6 first). */
void pack(const VoiceSpec &v, uint8_t out[128])
{
    memset(out, 0, 128);
    for (int op1 = 1; op1 <= 6; op1++) {
        const OpSpec &o = v.op[op1 - 1];
        uint8_t *p = out + (6 - op1) * 17;
        p[0] = o.r1; p[1] = o.r2; p[2] = o.r3; p[3] = o.r4;
        p[4] = o.l1; p[5] = o.l2; p[6] = o.l3; p[7] = o.l4;
        p[8] = 39;                                     // level-scaling break point C3
        p[9] = 0; p[10] = 0;                           // no left/right depth
        p[11] = 0;                                     // curves -LIN / -LIN
        p[12] = (uint8_t)((o.detune << 3) | (o.rs & 7));
        p[13] = (uint8_t)((o.kvs << 2) | (o.ams & 3));
        p[14] = o.ol;
        p[15] = (uint8_t)(o.coarse << 1);              // ratio mode (bit 0 = 0)
        p[16] = o.fine;
    }
    for (int i = 0; i < 4; i++) { out[102 + i] = 99; out[106 + i] = 50; }   // pitch envelope: flat
    out[110] = (uint8_t)(v.alg - 1);
    out[111] = (uint8_t)((1 << 3) | (v.fb & 7));        // oscillator sync on, feedback
    out[112] = v.lfs; out[113] = v.lfd; out[114] = v.lpmd; out[115] = v.lamd;
    out[116] = (uint8_t)((v.lpms << 4) | (v.lfw << 1)); // LFO key sync off
    out[117] = v.transpose;
    memset(out + 118, ' ', 10);
    memcpy(out + 118, v.name, strlen(v.name) > 10 ? 10 : strlen(v.name));
}

} // namespace

const uint8_t *factory()
{
    static uint8_t bank[BANK_BYTES];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 32; i++) pack(VOICES[i], bank + 128 * i);
        built = true;
    }
    return bank;
}

} // namespace synth_bank
