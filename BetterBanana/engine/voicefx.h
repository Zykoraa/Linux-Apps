// betterbanana - the voice changer.
//
// Header-only, float32, allocation-free: everything here is called from the
// PipeWire realtime thread, like dsp.h. Each effect has an "off" state it
// checks first, so an inactive rack costs one branch per sample.
#pragma once

#include "dsp.h"
#include "../common/protocol.h"

#include <cmath>

namespace bb {

// ---------------------------------------------------------------------------
// Delay-line pitch shifter.
//
// Two read heads chase the write head at the pitch ratio, half a window apart,
// crossfaded so whichever one is about to run off the end of its window is
// already silent. This is the classic granular shifter: cheap, allocation-free
// and no FFT on the audio thread. The price is a faint warble on speech that a
// phase vocoder would not have, at a grain rate of |ratio-1| * sr / window -
// about 8 Hz for a five-semitone shift, 23 Hz for an octave.
//
// To pitch UP the read head must move through the buffer faster than the write
// head, which would mean reading samples that do not exist yet. So it starts a
// whole window behind and catches up, wrapping back when it arrives: the delay
// falls at (ratio - 1) samples per sample.
//
// At zero semitones the heads sit at fixed offsets and their sum is a comb
// filter, not a copy - so zero bypasses rather than running the maths.
// ---------------------------------------------------------------------------
constexpr int kPitchWindow = 2048;              // 43 ms at 48 kHz
constexpr int kPitchBuf    = kPitchWindow * 2;  // max delay is 1.5 windows
constexpr float kPitchXfade = 0.25f;            // fraction of the window spent blending

struct PitchShifter {
    float buf[kPitchBuf] = {};
    int   wr = 0;
    float phase = 0.0f;                 // current delay, in samples
    float ratio = 1.0f;
    bool  active = false;

    // The head sweeps this range; both it and its crossfade partner (one whole
    // window away) stay inside the buffer and never ask for a future sample.
    static constexpr float kLo = kPitchWindow * 0.5f;
    static constexpr float kHi = kPitchWindow * 1.5f;

    void set_semitones(float st)
    {
        const bool was = active;
        active = std::fabs(st) > 0.01f;
        ratio = std::pow(2.0f, clampf(st, -24.0f, 24.0f) / 12.0f);
        // Coming back on with a buffer full of stale audio would splutter.
        if (active && !was) reset();
    }

    void reset()
    {
        for (int i = 0; i < kPitchBuf; ++i) buf[i] = 0.0f;
        phase = kPitchWindow;
    }

    inline float tap(float delay) const
    {
        float p = float(wr) - delay;
        while (p < 0.0f) p += float(kPitchBuf);
        const int i0 = int(p) % kPitchBuf;
        const int i1 = (i0 + 1) % kPitchBuf;
        const float f = p - std::floor(p);
        return buf[i0] * (1.0f - f) + buf[i1] * f;
    }

    inline float process(float x)
    {
        buf[wr] = x;
        wr = (wr + 1) % kPitchBuf;
        if (!active) return x;

        phase += 1.0f - ratio;              // ratio > 1: the head catches up

        // How close the head is to the end of its travel, and where it will
        // reappear. Pitching up runs the delay down to kLo and jumps it a whole
        // window back; pitching down does the mirror image.
        float dist, other;
        if (ratio > 1.0f) {
            if (phase < kLo) phase += float(kPitchWindow);
            dist  = phase - kLo;
            other = phase + float(kPitchWindow);
        } else {
            if (phase >= kHi) phase -= float(kPitchWindow);
            dist  = kHi - phase;
            other = phase - float(kPitchWindow);
        }

        // Only blend near the seam. Holding a single head for most of the
        // window is what keeps the pitched tone intact: two heads summed the
        // whole time sit at a fixed delay apart, so they cancel each other at
        // whatever frequency that offset happens to be a half period of, and
        // the result is audibly hollow.
        // The partner tap is only ever read inside the seam, which bounds the
        // largest delay at kHi - zone + kPitchWindow = 3584 samples - inside
        // the 4096-sample buffer. Widening kPitchXfade past 0.5 would break
        // that, so it stays a quarter.
        const float zone = kPitchXfade * float(kPitchWindow);
        if (dist >= zone) return tap(phase);

        const float b = 1.0f - dist / zone;             // 0 .. 1 across the seam
        const float a = b * kPi * 0.5f;
        return tap(phase) * std::cos(a) + tap(other) * std::sin(a);
    }
};

// ---------------------------------------------------------------------------
struct RingMod {
    float phase = 0.0f, inc = 0.0f, mix = 0.0f;
    bool  active = false;

    void configure(float sr, float hz, float m)
    {
        active = hz > 0.5f && m > 0.001f;
        inc = 2.0f * kPi * hz / sr;
        mix = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        if (!active) return x;
        phase += inc;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        return x * (1.0f - mix) + x * std::sin(phase) * mix;
    }
    void reset() { phase = 0.0f; }
};

// ---------------------------------------------------------------------------
// Bit depth and sample rate, separately: one gives you quantisation grit, the
// other the aliasing whine of an old sampler.
struct Crusher {
    int   bits = 0;                     // 0 = off
    int   down = 1;                     // 1 = off
    int   count = 0;
    float held = 0.0f;

    inline float process(float x)
    {
        if (down > 1) {
            if (count == 0) held = x;
            if (++count >= down) count = 0;
            x = held;
        }
        if (bits >= 2 && bits < 16) {
            const float levels = float(1 << (bits - 1));
            x = std::round(x * levels) / levels;
        }
        return x;
    }
    void reset() { count = 0; held = 0.0f; }
};

// ---------------------------------------------------------------------------
constexpr int kChorusBuf = 4096;        // 85 ms at 48 kHz

struct Chorus {
    float buf[kChorusBuf] = {};
    int   wr = 0;
    float phase = 0.0f, inc = 0.0f;
    float base = 0.0f, depth = 0.0f;    // samples
    float mix = 0.0f;
    bool  active = false;

    void configure(float sr, float depth_ms, float hz, float m)
    {
        active = depth_ms > 0.01f && m > 0.001f;
        base  = 0.015f * sr;                                  // 15 ms centre
        depth = clampf(depth_ms, 0.0f, 12.0f) * 0.001f * sr;
        inc   = 2.0f * kPi * clampf(hz, 0.01f, 8.0f) / sr;
        mix   = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        buf[wr] = x;
        wr = (wr + 1) % kChorusBuf;
        if (!active) return x;
        phase += inc;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        const float d = base + depth * std::sin(phase);
        float p = float(wr) - d;
        while (p < 0.0f) p += float(kChorusBuf);
        const int i0 = int(p) % kChorusBuf;
        const int i1 = (i0 + 1) % kChorusBuf;
        const float f = p - std::floor(p);
        const float wet = buf[i0] * (1.0f - f) + buf[i1] * f;
        return x * (1.0f - mix * 0.5f) + wet * mix;
    }
    void reset() { for (int i = 0; i < kChorusBuf; ++i) buf[i] = 0.0f; wr = 0; phase = 0.0f; }
};

// ---------------------------------------------------------------------------
constexpr int kEchoBuf = 48000;         // 1 s at 48 kHz

struct Echo {
    float buf[kEchoBuf] = {};
    int   wr = 0, delay = 0;
    float fb = 0.0f, mix = 0.0f;
    bool  active = false;

    void configure(float sr, float ms, float feedback, float m)
    {
        active = ms > 1.0f && m > 0.001f;
        delay = (int)clampf(ms * 0.001f * sr, 1.0f, float(kEchoBuf - 1));
        fb  = clampf(feedback, 0.0f, 0.95f);
        mix = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        if (!active) return x;
        int rd = wr - delay;
        if (rd < 0) rd += kEchoBuf;
        const float d = buf[rd];
        buf[wr] = x + d * fb;
        wr = (wr + 1) % kEchoBuf;
        return x + d * mix;
    }
    void reset() { for (int i = 0; i < kEchoBuf; ++i) buf[i] = 0.0f; wr = 0; }
};

// ---------------------------------------------------------------------------
// tanh saturation, normalised so turning it up changes the shape rather than
// just the level.
inline float drive_shape(float x, float k)
{
    return std::tanh(x * k) / std::tanh(k);
}

// ---------------------------------------------------------------------------
// The whole rack for one strip. Mirrors EqChain: cached parameters, an
// update() off the shm block, and a per-sample process().
// ---------------------------------------------------------------------------
struct VoiceFxChain {
    PitchShifter pitch[kChan];
    RingMod      ring[kChan];
    Crusher      crush[kChan];
    Chorus       chorus[kChan];
    Echo         echo[kChan];
    SmoothGain   out[kChan];
    float        drive_k = 1.0f;
    bool         drive_on = false;

    float c_pitch = 1e9f, c_drive = -1.0f;
    float c_rhz = -1.0f, c_rmix = -1.0f;
    int   c_bits = -1, c_down = -1;
    float c_ems = -1.0f, c_efb = -1.0f, c_emix = -1.0f;
    float c_cms = -1.0f, c_chz = -1.0f, c_cmix = -1.0f;
    bool  init = false;

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) {
            out[c].configure(sr, 15.0f);
            out[c].snap(1.0f);
            pitch[c].reset();
        }
    }

    void update(const VoiceFx& p, float sr)
    {
        const float st   = p.pitch.load(std::memory_order_relaxed);
        const float dr   = p.drive.load(std::memory_order_relaxed);
        const float rhz  = p.ring_hz.load(std::memory_order_relaxed);
        const float rmix = p.ring_mix.load(std::memory_order_relaxed);
        const int   bits = p.bits.load(std::memory_order_relaxed);
        const int   down = p.downsample.load(std::memory_order_relaxed);
        const float ems  = p.echo_ms.load(std::memory_order_relaxed);
        const float efb  = p.echo_fb.load(std::memory_order_relaxed);
        const float emix = p.echo_mix.load(std::memory_order_relaxed);
        const float cms  = p.chorus_ms.load(std::memory_order_relaxed);
        const float chz  = p.chorus_hz.load(std::memory_order_relaxed);
        const float cmix = p.chorus_mix.load(std::memory_order_relaxed);

        for (int c = 0; c < kChan; ++c) {
            if (!init || st != c_pitch) pitch[c].set_semitones(st);
            if (!init || rhz != c_rhz || rmix != c_rmix) ring[c].configure(sr, rhz, rmix);
            if (!init || bits != c_bits || down != c_down) {
                crush[c].bits = bits;
                crush[c].down = down < 1 ? 1 : down;
            }
            if (!init || ems != c_ems || efb != c_efb || emix != c_emix)
                echo[c].configure(sr, ems, efb, emix);
            if (!init || cms != c_cms || chz != c_chz || cmix != c_cmix)
                chorus[c].configure(sr, cms, chz, cmix);
            out[c].set_target(db_to_lin(p.gain_db.load(std::memory_order_relaxed)));
        }
        if (!init || dr != c_drive) {
            drive_on = dr > 0.01f;
            drive_k = 1.0f + clampf(dr, 0.0f, 10.0f) * 1.5f;
        }
        c_pitch = st; c_drive = dr;
        c_rhz = rhz; c_rmix = rmix;
        c_bits = bits; c_down = down;
        c_ems = ems; c_efb = efb; c_emix = emix;
        c_cms = cms; c_chz = chz; c_cmix = cmix;
        init = true;
    }

    inline float process(int c, float x)
    {
        x = pitch[c].process(x);
        if (drive_on) x = drive_shape(x, drive_k);
        x = ring[c].process(x);
        x = crush[c].process(x);
        x = chorus[c].process(x);
        x = echo[c].process(x);
        return x * out[c].next();
    }
};

} // namespace bb
