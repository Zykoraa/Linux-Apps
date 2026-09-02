// betterbanana - pitch correction.
//
// Autotune is a control loop, not a new effect: the period tracker already
// measures the singer's pitch in the realtime path, and the pitch shifter
// already takes a ratio. This is the bit in the middle - work out which note
// they were aiming at, and how far off they landed.
#pragma once

#include "dsp.h"

#include <cmath>

namespace bb {

enum TuneScale : int32_t { kTuneChromatic = 0, kTuneMajor, kTuneMinor, kTuneScaleCount };

// Correction in semitones that moves `hz` onto the nearest note of the scale.
// `key` is the tonic as a pitch class, 0 = C. Chromatic ignores it.
//
// Returns 0 for anything outside a sensible vocal range rather than dragging a
// misdetected octave somewhere absurd.
inline float tune_correction(float hz, int key, int scale)
{
    if (!(hz > 25.0f) || hz > 2000.0f) return 0.0f;

    // MIDI numbering, fractional: 69 is A4 = 440 Hz, and one step is a semitone.
    const double note = 69.0 + 12.0 * std::log2(double(hz) / 440.0);
    if (scale == kTuneChromatic)
        return float(std::lround(note) - note);

    static const int kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int kMinor[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* iv = (scale == kTuneMinor) ? kMinor : kMajor;

    const int centre = (int)std::lround(note);
    double best = note, bd = 1e9;
    // Scanning upward with a strict comparison means an exact tie - a note
    // sitting midway between two scale degrees - always resolves downward.
    // Either is musically defensible; being consistent is what matters, since
    // a tie that wobbled would make the correction chatter.
    // Half an octave either side is more than enough; the nearest scale note is
    // never more than two semitones away.
    for (int c = centre - 6; c <= centre + 6; ++c) {
        const int pc = ((c - key) % 12 + 12) % 12;
        bool in = false;
        for (int i = 0; i < 7; ++i) if (iv[i] == pc) { in = true; break; }
        if (!in) continue;
        const double d = std::fabs(double(c) - note);
        if (d < bd) { bd = d; best = c; }
    }
    return float(best - note);
}

// The note a correction lands on, for display.
inline int tune_note_of(float hz, float correction)
{
    if (!(hz > 25.0f)) return -1;
    return (int)std::lround(69.0 + 12.0 * std::log2(double(hz) / 440.0) + correction);
}

inline const char* tune_note_name(int midi)
{
    static const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B" };
    if (midi < 0) return "-";
    return kNames[((midi % 12) + 12) % 12];
}

} // namespace bb
