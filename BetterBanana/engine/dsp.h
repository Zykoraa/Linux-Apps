// betterbanana - DSP primitives
// Header-only, float32, allocation-free. Everything here is safe to call
// from the PipeWire realtime thread.
#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace bb {

constexpr float kPi = 3.14159265358979323846f;

// Flush-to-zero / denormals-are-zero. Call once per realtime thread.
inline void enable_ftz()
{
#if defined(__SSE__) || defined(__x86_64__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

inline float db_to_lin(float db) { return db <= -120.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline float lin_to_db(float x)  { return x <= 1e-9f ? -180.0f : 20.0f * std::log10(x); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Biquad, transposed direct form II (good numerical behaviour at low freq).
// ---------------------------------------------------------------------------
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0.0f; }

    inline float process(float x)
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set_bypass() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    // Normalise an unnormalised biquad (a0 divided out).
    void set_coeffs(float B0, float B1, float B2, float A0, float A1, float A2)
    {
        const float inv = 1.0f / A0;
        b0 = B0 * inv; b1 = B1 * inv; b2 = B2 * inv;
        a1 = A1 * inv; a2 = A2 * inv;
    }

    // RBJ cookbook designers. gain_db is used by the shelf/peak types only.
    void set_peaking(float sr, float freq, float q, float gain_db)
    {
        if (std::fabs(gain_db) < 1e-4f) { set_bypass(); return; }
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w = 2.0f * kPi * clampf(freq, 10.0f, sr * 0.49f) / sr;
        const float alpha = std::sin(w) / (2.0f * std::max(q, 0.05f));
        const float cw = std::cos(w);
        set_coeffs(1 + alpha * A, -2 * cw, 1 - alpha * A,
                   1 + alpha / A, -2 * cw, 1 - alpha / A);
    }

    void set_lowshelf(float sr, float freq, float q, float gain_db)
    {
        if (std::fabs(gain_db) < 1e-4f) { set_bypass(); return; }
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w = 2.0f * kPi * clampf(freq, 10.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        const float tsa = 2.0f * std::sqrt(A) * alpha;
        set_coeffs(A * ((A + 1) - (A - 1) * cw + tsa),
                   2 * A * ((A - 1) - (A + 1) * cw),
                   A * ((A + 1) - (A - 1) * cw - tsa),
                   (A + 1) + (A - 1) * cw + tsa,
                   -2 * ((A - 1) + (A + 1) * cw),
                   (A + 1) + (A - 1) * cw - tsa);
    }

    void set_highshelf(float sr, float freq, float q, float gain_db)
    {
        if (std::fabs(gain_db) < 1e-4f) { set_bypass(); return; }
        const float A = std::pow(10.0f, gain_db / 40.0f);
        const float w = 2.0f * kPi * clampf(freq, 10.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        const float tsa = 2.0f * std::sqrt(A) * alpha;
        set_coeffs(A * ((A + 1) + (A - 1) * cw + tsa),
                   -2 * A * ((A - 1) + (A + 1) * cw),
                   A * ((A + 1) + (A - 1) * cw - tsa),
                   (A + 1) - (A - 1) * cw + tsa,
                   2 * ((A - 1) - (A + 1) * cw),
                   (A + 1) - (A - 1) * cw - tsa);
    }

    void set_highpass(float sr, float freq, float q)
    {
        const float w = 2.0f * kPi * clampf(freq, 5.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        set_coeffs((1 + cw) * 0.5f, -(1 + cw), (1 + cw) * 0.5f,
                   1 + alpha, -2 * cw, 1 - alpha);
    }

    void set_lowpass(float sr, float freq, float q)
    {
        const float w = 2.0f * kPi * clampf(freq, 5.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        set_coeffs((1 - cw) * 0.5f, 1 - cw, (1 - cw) * 0.5f,
                   1 + alpha, -2 * cw, 1 - alpha);
    }

    // Constant-skirt-gain notch and band-pass, for completeness: Equalizer APO
    // profiles occasionally use them, so an import must not silently drop one.
    void set_notch(float sr, float freq, float q)
    {
        const float w = 2.0f * kPi * clampf(freq, 5.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        set_coeffs(1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void set_bandpass(float sr, float freq, float q)
    {
        const float w = 2.0f * kPi * clampf(freq, 5.0f, sr * 0.49f) / sr;
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * std::max(q, 0.05f));
        set_coeffs(alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
    }

    // Magnitude response at `freq`, in dB. Used by the GUI's EQ curve and tests.
    float magnitude_db(float sr, float freq) const
    {
        const double w = 2.0 * M_PI * freq / sr;
        const double cw = std::cos(w), c2w = std::cos(2 * w);
        const double sw = std::sin(w), s2w = std::sin(2 * w);
        const double nr = b0 + b1 * cw + b2 * c2w, ni = -(b1 * sw + b2 * s2w);
        const double dr = 1.0 + a1 * cw + a2 * c2w, di = -(a1 * sw + a2 * s2w);
        const double n2 = nr * nr + ni * ni, d2 = dr * dr + di * di;
        if (d2 < 1e-30) return 0.0f;
        return static_cast<float>(10.0 * std::log10(n2 / d2));
    }
};

// Designs a band from an EqFilterType. Kept here rather than in the engine so
// the GUI's curve and the engine's audio path can never disagree about what a
// given band does. `type` is a bb::EqFilterType; the header does not include
// protocol.h, so it is taken as a plain int.
inline void design_band(Biquad& bq, int type, float sr, float freq, float q, float gain_db)
{
    switch (type) {
        case 1:  bq.set_lowshelf (sr, freq, q, gain_db); break;
        case 2:  bq.set_highshelf(sr, freq, q, gain_db); break;
        case 3:  bq.set_highpass (sr, freq, q);          break;
        case 4:  bq.set_lowpass  (sr, freq, q);          break;
        case 5:  bq.set_notch    (sr, freq, q);          break;
        case 6:  bq.set_bandpass (sr, freq, q);          break;
        default: bq.set_peaking  (sr, freq, q, gain_db); break;
    }
}

// ---------------------------------------------------------------------------
// One-pole smoothed gain. Removes zipper noise when a fader moves.
// ---------------------------------------------------------------------------
struct SmoothGain {
    float current = 0.0f, target = 0.0f, coeff = 0.0f;

    void configure(float sr, float ms)
    {
        coeff = std::exp(-1.0f / (std::max(ms, 0.01f) * 0.001f * sr));
    }
    void set_target(float t) { target = t; }
    void snap(float t) { current = target = t; }
    inline float next()
    {
        current = target + (current - target) * coeff;
        return current;
    }
};

// ---------------------------------------------------------------------------
// Peak meter with decay and hold, matching Voicemeeter's ballistics closely
// enough to feel right. Fed post-fader, read by the GUI via shared memory.
// ---------------------------------------------------------------------------
struct PeakMeter {
    float peak = 0.0f;       // decaying display value, linear
    float hold = 0.0f;       // peak-hold value, linear
    float decay_coeff = 0.0f;
    int   hold_samples = 0;
    int   hold_counter = 0;

    void configure(float sr, float decay_ms = 300.0f, float hold_ms = 1500.0f)
    {
        decay_coeff  = std::exp(-1.0f / (decay_ms * 0.001f * sr));
        hold_samples = static_cast<int>(hold_ms * 0.001f * sr);
    }

    // Feed a whole block; cheaper than per-sample and plenty accurate.
    inline void feed(const float* buf, uint32_t n)
    {
        float m = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            const float a = std::fabs(buf[i]);
            if (a > m) m = a;
        }
        if (m > peak) peak = m;
        else peak *= std::pow(decay_coeff, static_cast<float>(n));

        if (m >= hold) { hold = m; hold_counter = hold_samples; }
        else {
            hold_counter -= static_cast<int>(n);
            if (hold_counter <= 0) { hold = peak; hold_counter = 0; }
        }
    }

    // Feed a single already-computed block peak, decaying as if `n` samples
    // had elapsed. Used by the mixer, which finds the peak inline.
    inline void feed_peak(float m, uint32_t n)
    {
        if (m > peak) peak = m;
        else peak *= std::pow(decay_coeff, static_cast<float>(n));

        if (m >= hold) { hold = m; hold_counter = hold_samples; }
        else {
            hold_counter -= static_cast<int>(n);
            if (hold_counter <= 0) { hold = peak; hold_counter = 0; }
        }
    }

    void reset() { peak = hold = 0.0f; hold_counter = 0; }
};

// ---------------------------------------------------------------------------
// Noise gate. Driven by Voicemeeter's single 0..10 "Gate" knob, which we map
// onto threshold; timing constants are fixed the way the original's are.
// ---------------------------------------------------------------------------
struct Gate {
    float threshold = 0.0f;   // linear
    float attack_c = 0.0f, release_c = 0.0f, env_c = 0.0f;
    float env = 0.0f, gain = 1.0f;
    int   hold_samples = 0, hold_counter = 0;
    bool  enabled = false;

    void configure(float sr)
    {
        attack_c     = std::exp(-1.0f / (0.001f  * sr));   // 1 ms
        release_c    = std::exp(-1.0f / (0.150f  * sr));   // 150 ms
        env_c        = std::exp(-1.0f / (0.010f  * sr));   // 10 ms detector
        hold_samples = static_cast<int>(0.030f * sr);      // 30 ms
    }

    // knob: 0 = off .. 10 = maximally aggressive
    void set_knob(float knob)
    {
        enabled = knob > 0.01f;
        // Switching off must also clear the running state: process() returns
        // early when disabled, so a stale `gain` would keep being reported as
        // gain reduction long after the gate stopped doing anything.
        if (!enabled) { gain = 1.0f; env = 0.0f; hold_counter = 0; }
        threshold = db_to_lin(-60.0f + clampf(knob, 0.0f, 10.0f) * 4.0f);
    }

    inline float process(float x)
    {
        if (!enabled) return x;
        const float a = std::fabs(x);
        env = (a > env) ? a : (a + (env - a) * env_c);

        float t;
        if (env > threshold) { t = 1.0f; hold_counter = hold_samples; }
        else if (hold_counter > 0) { t = 1.0f; --hold_counter; }
        else t = 0.0f;

        const float c = (t > gain) ? attack_c : release_c;
        gain = t + (gain - t) * c;
        return x * gain;
    }

    void reset() { env = 0.0f; gain = 1.0f; hold_counter = 0; }
};

// ---------------------------------------------------------------------------
// Feed-forward compressor with auto makeup, driven by the single 0..10 "Comp"
// knob: turning it up simultaneously lowers threshold and raises ratio.
// ---------------------------------------------------------------------------
struct Compressor {
    float threshold_db = 0.0f, ratio = 1.0f, makeup = 1.0f;
    float attack_c = 0.0f, release_c = 0.0f;
    float env_db = 0.0f, gr_db = 0.0f;
    bool  enabled = false;
    static constexpr float kKnee = 6.0f;   // dB, soft knee width

    void configure(float sr)
    {
        attack_c  = std::exp(-1.0f / (0.005f * sr));   // 5 ms
        release_c = std::exp(-1.0f / (0.100f * sr));   // 100 ms
    }

    void set_knob(float knob)
    {
        const float k = clampf(knob, 0.0f, 10.0f);
        enabled = k > 0.01f;
        if (!enabled) { gr_db = 0.0f; env_db = 0.0f; }   // see Gate::set_knob
        threshold_db = -k * 3.0f;          //   0 .. -30 dB
        ratio        = 1.0f + k * 0.8f;    //   1 ..   9 :1
        // Auto makeup: restore roughly what the threshold/ratio took away.
        makeup = db_to_lin(-threshold_db * (1.0f - 1.0f / ratio) * 0.6f);
    }

    inline float process(float x)
    {
        if (!enabled) return x;
        const float a = std::fabs(x);
        const float in_db = a <= 1e-9f ? -180.0f : 20.0f * std::log10(a);

        // Soft-knee static curve -> desired gain reduction in dB.
        float over = in_db - threshold_db;
        float want = 0.0f;
        if (over >= kKnee * 0.5f) {
            want = over * (1.0f / ratio - 1.0f);
        } else if (over > -kKnee * 0.5f) {
            const float t = over + kKnee * 0.5f;
            want = (1.0f / ratio - 1.0f) * (t * t) / (2.0f * kKnee);
        }

        // Smooth in the dB domain; attack when clamping down harder.
        const float c = (want < gr_db) ? attack_c : release_c;
        gr_db = want + (gr_db - want) * c;

        return x * db_to_lin(gr_db) * makeup;
    }

    void reset() { env_db = 0.0f; gr_db = 0.0f; }
};

// ---------------------------------------------------------------------------
// Constant-power stereo pan. x in [-1, 1].
// ---------------------------------------------------------------------------
inline void pan_gains(float x, float& gl, float& gr)
{
    const float t = (clampf(x, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;  // 0..pi/2
    gl = std::cos(t);
    gr = std::sin(t);
}

} // namespace bb
