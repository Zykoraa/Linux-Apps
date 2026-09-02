// betterbanana - measuring the pitch of a recorded voice.
//
// Offline analysis, not realtime: the calibration wizard records a few seconds
// and asks this where the speaker's voice actually sits, because the right
// pitch shift is entirely a function of where you are starting from. A preset
// that lifts six semitones lands a 95 Hz voice at 134 Hz and a 140 Hz voice at
// 198 Hz - the same setting, two completely different results.
//
// Method: normalised square difference (McLeod), which is autocorrelation with
// an energy normalisation that stops it drifting toward zero lag. Peaks are
// picked lowest-lag-first among the strong ones, because the classic failure of
// autocorrelation is locking onto twice the true period and reporting an octave
// too low.
#pragma once

#include "formant.h"   // ifft()

#include <algorithm>
#include <cmath>
#include <vector>

namespace bb {

constexpr int kPtFrame = 2048;      // 43 ms: two periods of even a very low voice
constexpr int kPtHop   = 1024;
constexpr int kPtFft   = 4096;      // zero-padded, so the correlation is linear

struct PitchEstimate {
    float median_hz = 0.0f;
    float low_hz    = 0.0f;         // 10th percentile of the voiced frames
    float high_hz   = 0.0f;         // 90th
    int   voiced    = 0;
    int   frames    = 0;
    float clarity   = 0.0f;         // mean peak strength, 0..1
    bool  ok() const { return voiced >= 8 && median_hz > 0.0f; }
};

// One frame; returns 0 if it does not look voiced.
inline float pt_frame_hz(const float* x, float sr, float fmin, float fmax,
                         float* clarity_out)
{
    static thread_local std::vector<float> re, im, win, buf;
    if (win.size() != (size_t)kPtFrame) {
        win.resize(kPtFrame);
        for (int i = 0; i < kPtFrame; ++i)
            win[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (kPtFrame - 1)));
    }
    re.assign(kPtFft, 0.0f);
    im.assign(kPtFft, 0.0f);
    buf.assign(kPtFrame, 0.0f);

    double mean = 0.0;
    for (int i = 0; i < kPtFrame; ++i) mean += x[i];
    mean /= kPtFrame;
    double energy = 0.0;
    for (int i = 0; i < kPtFrame; ++i) {
        buf[i] = float((x[i] - mean) * win[i]);          // DC out, taper the ends
        energy += double(buf[i]) * buf[i];
        re[i] = buf[i];
    }
    if (clarity_out) *clarity_out = 0.0f;
    if (energy < 1e-7) return 0.0f;                      // silence

    // Autocorrelation as the inverse transform of the power spectrum.
    fft(re.data(), im.data(), kPtFft);
    for (int k = 0; k < kPtFft; ++k) {
        const float p = re[k] * re[k] + im[k] * im[k];
        re[k] = p; im[k] = 0.0f;
    }
    ifft(re.data(), im.data(), kPtFft);

    // Normalised square difference: divide by the energy actually overlapping
    // at each lag, so a long lag is not penalised for seeing fewer samples.
    const int lo = std::max(2, int(sr / fmax));
    const int hi = std::min(kPtFrame - 2, int(sr / fmin));
    if (hi <= lo + 2) return 0.0f;

    std::vector<float> n(hi + 2, 0.0f);
    double m = 2.0 * energy;
    for (int tau = 1; tau <= hi + 1; ++tau) {
        m -= double(buf[kPtFrame - tau]) * buf[kPtFrame - tau];
        m -= double(buf[tau - 1]) * buf[tau - 1];
        n[tau] = m > 1e-12 ? float(2.0 * re[tau] / m) : 0.0f;
    }

    float best = 0.0f;
    for (int tau = lo; tau <= hi; ++tau) best = std::max(best, n[tau]);
    if (best < 0.35f) return 0.0f;                       // nothing periodic here

    // Lowest lag among the strong peaks. Multiples of the true period also peak,
    // so taking the global maximum is how you end up an octave low.
    int pick = -1;
    for (int tau = lo + 1; tau < hi; ++tau) {
        if (n[tau] <= n[tau - 1] || n[tau] < n[tau + 1]) continue;
        if (n[tau] >= 0.85f * best) { pick = tau; break; }
    }
    if (pick < 0) return 0.0f;

    // Parabolic interpolation, so the answer is not quantised to whole samples.
    const float a = n[pick - 1], b = n[pick], c = n[pick + 1];
    const float den = a - 2 * b + c;
    const float off = std::fabs(den) < 1e-9f ? 0.0f : 0.5f * (a - c) / den;
    if (clarity_out) *clarity_out = b;
    return sr / (float(pick) + off);
}

inline PitchEstimate estimate_pitch(const float* x, int n, float sr,
                                    float fmin = 60.0f, float fmax = 500.0f)
{
    PitchEstimate e;
    std::vector<float> hz;
    double clar = 0.0;
    for (int off = 0; off + kPtFrame <= n; off += kPtHop) {
        ++e.frames;
        float c = 0.0f;
        const float f = pt_frame_hz(x + off, sr, fmin, fmax, &c);
        if (f > 0.0f) { hz.push_back(f); clar += c; }
    }
    e.voiced = (int)hz.size();
    if (hz.empty()) return e;
    std::sort(hz.begin(), hz.end());
    auto pct = [&](double p) {
        const double idx = p * (hz.size() - 1);
        const size_t i = (size_t)idx;
        const double f = idx - double(i);
        return i + 1 < hz.size() ? float(hz[i] * (1 - f) + hz[i + 1] * f) : hz[i];
    };
    e.median_hz = pct(0.5);
    e.low_hz    = pct(0.1);
    e.high_hz   = pct(0.9);
    e.clarity   = float(clar / hz.size());
    return e;
}

// The shift, in semitones, that moves `from` onto `to`.
inline float semitones_between(float from, float to)
{
    if (from <= 0.0f || to <= 0.0f) return 0.0f;
    return 12.0f * std::log2(to / from);
}

} // namespace bb
