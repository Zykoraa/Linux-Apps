// betterbanana - formant shifting.
//
// The pitch shifter next door is a resampler with the duration patched up, so
// it moves pitch and formants together: that is why a big shift sounds like a
// small person rather than a different one. This moves the spectral envelope
// on its own, which is what actually carries perceived body size.
//
// Method: short-time Fourier transform, split each frame's magnitude into a
// smooth envelope (the formants) and everything else (the harmonics, which
// carry pitch), stretch the envelope along the frequency axis, put it back.
// The phase is never touched, which is why this does not get the smeared,
// phasey quality a full phase vocoder has - we are only warping something
// smooth.
//
// Allocation-free and deterministic, but not cheap: about 4-5% of a core per
// active channel pair, and it adds one window of latency. It runs only when a
// shift is actually asked for.
#pragma once

#include "dsp.h"
#include "spectrum.h"

#include <cmath>

namespace bb {

constexpr int kFmtN    = 1024;      // 21 ms window, and the added latency
constexpr int kFmtHop  = 256;       // 4x overlap
constexpr int kFmtLift = 48;        // cepstral cutoff: envelope, not harmonics

// Inverse of the transform in spectrum.h. Conjugate, forward, conjugate, scale.
inline void ifft(float* re, float* im, int n)
{
    for (int i = 0; i < n; ++i) im[i] = -im[i];
    fft(re, im, n);
    const float s = 1.0f / float(n);
    for (int i = 0; i < n; ++i) { re[i] *= s; im[i] = -im[i] * s; }
}

struct FormantShifter {
    float win[kFmtN] = {};
    float inbuf[kFmtN] = {};        // circular; holds exactly one window
    float outbuf[kFmtN] = {};       // circular overlap-add accumulator
    int   wi = 0, hopc = 0;
    float ratio = 1.0f;
    bool  active = false, ready = false;

    // Scratch. Kept per instance rather than shared so nothing here depends on
    // which thread or which strip is running.
    float re[kFmtN] = {}, im[kFmtN] = {};
    float cre[kFmtN] = {}, cim[kFmtN] = {};
    float env[kFmtN] = {};

    // Hann analysis and synthesis at a quarter-window hop sums to exactly 1.5.
    static constexpr float kOla = 1.0f / 1.5f;

    void configure()
    {
        for (int i = 0; i < kFmtN; ++i)
            win[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (kFmtN - 1)));
        reset();
        ready = true;
    }

    void reset()
    {
        for (int i = 0; i < kFmtN; ++i) { inbuf[i] = 0.0f; outbuf[i] = 0.0f; }
        wi = 0; hopc = 0;
    }

    // `semitones` is how far the envelope moves, positive being up.
    void set_shift(float semitones)
    {
        const bool was = active;
        active = std::fabs(semitones) > 0.01f;
        ratio = std::pow(2.0f, clampf(semitones, -12.0f, 12.0f) / 12.0f);
        if (active && !was) { if (!ready) configure(); reset(); }
    }

    inline float process(float x)
    {
        if (!active) return x;
        inbuf[wi] = x;
        const float y = outbuf[wi];
        outbuf[wi] = 0.0f;
        wi = (wi + 1) % kFmtN;
        if (++hopc >= kFmtHop) { hopc = 0; frame(); }
        return y;
    }

    // One analysis/synthesis pass over the window that just filled.
    void frame()
    {
        const int half = kFmtN / 2;

        // wi now points at the oldest sample, so the window reads forward.
        for (int i = 0; i < kFmtN; ++i) {
            re[i] = inbuf[(wi + i) % kFmtN] * win[i];
            im[i] = 0.0f;
        }
        fft(re, im, kFmtN);

        // Log magnitude, made symmetric so its transform is the real cepstrum.
        for (int k = 0; k <= half; ++k) {
            const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]);
            cre[k] = std::log(m + 1e-12f);
            cim[k] = 0.0f;
        }
        for (int k = half + 1; k < kFmtN; ++k) { cre[k] = cre[kFmtN - k]; cim[k] = 0.0f; }

        fft(cre, cim, kFmtN);
        // Keep only low quefrency: the envelope. The harmonic comb of a voice
        // lives far higher up, so this is what separates formants from pitch.
        for (int q = kFmtLift; q < kFmtN - kFmtLift + 1; ++q) { cre[q] = 0.0f; cim[q] = 0.0f; }
        ifft(cre, cim, kFmtN);
        for (int k = 0; k <= half; ++k) env[k] = cre[k];        // still log domain

        // Stretch the envelope along frequency, then correct each bin by how
        // much the envelope moved there. Working in the log domain makes that
        // correction a subtraction, and bounding it stops a near-silent bin
        // from being amplified into a whistle.
        for (int k = 0; k <= half; ++k) {
            const float src = float(k) / ratio;
            float want;
            if (src >= float(half)) {
                want = env[half];
            } else {
                const int i0 = int(src);
                const float f = src - float(i0);
                want = env[i0] * (1.0f - f) + env[i0 + 1] * f;
            }
            const float g = std::exp(clampf(want - env[k], -3.5f, 3.5f));
            re[k] *= g; im[k] *= g;
            if (k > 0 && k < half) { re[kFmtN - k] *= g; im[kFmtN - k] *= g; }
        }

        ifft(re, im, kFmtN);
        for (int i = 0; i < kFmtN; ++i)
            outbuf[(wi + i) % kFmtN] += re[i] * win[i] * kOla;
    }
};

} // namespace bb
