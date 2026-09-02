// betterbanana - the spectrum analyser behind the EQ curve.
//
// Deliberately NOT realtime code. The mixer appends samples to a SpecTap from
// the audio thread; the engine's control thread copies a window out and runs
// this on it a few times a second. Keeping the transform off the audio thread
// is the whole point: an FFT in the process callback is how you get xruns.
#pragma once

#include "../common/protocol.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <atomic>

namespace bb {

// 4096 points at 48 kHz is 11.7 Hz per bin: fine enough that the lowest
// displayed band is only a few bins wide, and cheap enough to run at 20 fps.
constexpr int kSpecFft = 4096;

// ---------------------------------------------------------------------------
// Sliding capture window. The audio thread appends; the control thread copies
// the most recent kSpecFft frames out. A copy can race with a write and pick up
// a sample or two from the previous lap around the buffer; in a spectrum
// display that is invisible, and it keeps the audio thread free of any
// synchronisation at all.
// ---------------------------------------------------------------------------
struct SpecTap {
    static constexpr uint32_t kSize = 16384;      // power of two, >> kSpecFft
    float buf[kSize] = {};
    std::atomic<uint32_t> wr{0};

    // Appends the mono sum of an interleaved stereo block.
    void write_stereo(const float* lr, uint32_t frames)
    {
        uint32_t w = wr.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i)
            buf[(w + i) & (kSize - 1)] = 0.5f * (lr[i * kChan] + lr[i * kChan + 1]);
        wr.store(w + frames, std::memory_order_release);
    }

    void clear()
    {
        for (uint32_t i = 0; i < kSize; ++i) buf[i] = 0.0f;
    }

    // The newest `n` frames, oldest first.
    void snapshot(float* dst, uint32_t n) const
    {
        const uint32_t w = wr.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i) dst[i] = buf[(w - n + i) & (kSize - 1)];
    }
};

// ---------------------------------------------------------------------------
// Iterative in-place radix-2 complex FFT. `n` must be a power of two.
//
// Twiddles are recomputed per stage rather than carried by a recurrence, which
// costs about n trig calls for the whole transform - a few tens of microseconds
// - and avoids the phase drift a recurrence accumulates over twelve stages.
// ---------------------------------------------------------------------------
inline void fft(float* re, float* im, int n)
{
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const int half = len >> 1;
        for (int j = 0; j < half; ++j) {
            const float wr = (float)std::cos(ang * j);
            const float wi = (float)std::sin(ang * j);
            for (int i = 0; i < n; i += len) {
                const int a = i + j, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// One analyser: window, transform, fold into log-spaced bands, decay.
// Owns its scratch, so running it allocates nothing.
// ---------------------------------------------------------------------------
struct SpectrumAnalyzer {
    float re[kSpecFft] = {};
    float im[kSpecFft] = {};
    float win[kSpecFft] = {};
    float disp[kSpecBins] = {};      // decayed display values, dBFS
    float scale = 0.0f;              // magnitude -> amplitude, see configure()
    bool  ready = false;

    static constexpr float kFloorDb = -110.0f;

    void configure()
    {
        double energy = 0.0;
        for (int i = 0; i < kSpecFft; ++i) {
            win[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (kSpecFft - 1)));
            energy += (double)win[i] * win[i];
        }
        // Each band sums the POWER of every bin in it, so the window's energy
        // is what sets the calibration, not its coherent gain: a tone's power
        // is spread across its main lobe and we add all of it back up. This
        // constant makes a full-scale sine read exactly 0 dBFS.
        scale = (float)(2.0 / std::sqrt((double)kSpecFft * energy));
        reset();
        ready = true;
    }

    void reset()
    {
        for (int b = 0; b < kSpecBins; ++b) disp[b] = kFloorDb;
    }

    // Analyses one window and folds it into kSpecBins log-spaced bands between
    // f_lo and f_hi. Bands rise instantly and fall by `fall_db` per call, which
    // is what makes a spectrum readable rather than a flicker.
    //
    // Calibration: a full-scale sine reads 0 dBFS in the band that contains it.
    void analyze(const float* mono, float sr, float f_lo, float f_hi, float fall_db)
    {
        if (!ready) configure();
        for (int i = 0; i < kSpecFft; ++i) { re[i] = mono[i] * win[i]; im[i] = 0.0f; }
        fft(re, im, kSpecFft);

        const double binHz = sr / kSpecFft;
        const int    nyq   = kSpecFft / 2;
        const double ratio = std::pow((double)f_hi / f_lo, 1.0 / kSpecBins);

        for (int b = 0; b < kSpecBins; ++b) {
            const double lo = f_lo * std::pow(ratio, b);
            const double hi = lo * ratio;
            int j0 = (int)std::ceil(lo / binHz);
            int j1 = (int)std::floor(hi / binHz);
            if (j0 < 1) j0 = 1;
            if (j1 > nyq - 1) j1 = nyq - 1;

            double power = 0.0;
            if (j1 >= j0) {
                for (int j = j0; j <= j1; ++j) {
                    const double a = re[j] * scale, c = im[j] * scale;
                    power += a * a + c * c;
                }
            } else {
                // Below about 200 Hz a log band is narrower than one FFT bin,
                // so there is nothing whose centre falls inside it. Take the
                // nearest bin rather than reporting silence.
                int j = (int)std::lround(0.5 * (lo + hi) / binHz);
                if (j < 1) j = 1;
                if (j > nyq - 1) j = nyq - 1;
                const double a = re[j] * scale, c = im[j] * scale;
                power = a * a + c * c;
            }

            float db = power <= 1e-16 ? kFloorDb : (float)(10.0 * std::log10(power));
            if (db < kFloorDb) db = kFloorDb;
            disp[b] = db > disp[b] ? db : std::max(kFloorDb, disp[b] - fall_db);
        }
    }
};

} // namespace bb
