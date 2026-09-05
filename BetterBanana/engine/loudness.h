// betterbanana - loudness, ITU-R BS.1770-4.
//
// A peak meter says whether something will clip. It says nothing about how loud
// it will sound, which is the number every streaming platform actually measures
// and the reason two mixes that both peak at -1 dBFS can be six decibels apart
// to a listener.
//
// Three figures, all in LUFS (decibels, but on a loudness scale where the
// reference is the full-scale sine):
//   short-term   a rolling 3 second window - what to watch while talking
//   integrated   everything since the last reset, gated so silence between
//                sentences does not drag the number down
//
// The K-weighting filters are DERIVED from the analog specifications in the
// standard rather than pasted in as 48 kHz coefficients, so the meter is still
// correct if PipeWire negotiates 44.1 or 96 kHz. tests/test_loudness.cpp checks
// that at 48 kHz the derivation reproduces the published coefficients.
#pragma once

#include <cmath>
#include <cstring>

namespace bb {

// A biquad in direct form I, kept separate from engine/dsp.h's: that one is
// built for controls that move, and this one must not be touched once set.
struct LoudBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() { x1 = x2 = y1 = y2 = 0; }

    double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// Stage 1 of the K-weighting: a +4 dB high shelf standing in for the acoustic
// effect of a head in a sound field.
//
// Not the textbook RBJ shelf - that is a different filter and misses the
// published coefficients by about 0.4%, which is enough to put the meter a
// third of a decibel out. This is the standard's own formulation, in terms of
// K = tan(pi*f0/fs), which is what makes it reproduce the published 48 kHz
// numbers exactly and stay correct at every other rate.
inline LoudBiquad k_shelf(double sr)
{
    const double db = 3.999843853973347;
    const double f0 = 1681.974450955533;
    const double Q  = 0.7071752369554196;

    const double K  = std::tan(M_PI * f0 / sr);
    const double Vh = std::pow(10.0, db / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;

    LoudBiquad f;
    f.b0 = (Vh + Vb * K / Q + K * K) / a0;
    f.b1 = 2.0 * (K * K - Vh)        / a0;
    f.b2 = (Vh - Vb * K / Q + K * K) / a0;
    f.a1 = 2.0 * (K * K - 1.0)       / a0;
    f.a2 = (1.0 - K / Q + K * K)     / a0;
    return f;
}

// Stage 2: a high-pass that discards the rumble no one hears as level.
//
// The numerator is literally 1, -2, 1 and is deliberately NOT normalised by a0
// - that is how the standard specifies it, and it leaves the filter with about
// +0.04 dB of passband gain. Normalising it "tidily" is a real error of that
// size in every reading.
inline LoudBiquad k_highpass(double sr)
{
    const double f0 = 38.13547087602444;
    const double Q  = 0.5003270373238773;

    const double K  = std::tan(M_PI * f0 / sr);
    const double a0 = 1.0 + K / Q + K * K;

    LoudBiquad f;
    f.b0 =  1.0;
    f.b1 = -2.0;
    f.b2 =  1.0;
    f.a1 = 2.0 * (K * K - 1.0)   / a0;
    f.a2 = (1.0 - K / Q + K * K) / a0;
    return f;
}

class Loudness {
public:
    // The standard's own numbers: a gating block is 400 ms stepped every 100 ms,
    // which is four 100 ms sub-blocks with 75% overlap.
    static constexpr int   kSubMs      = 100;
    static constexpr int   kGateSubs   = 4;    // 400 ms
    static constexpr int   kShortSubs  = 30;   // 3 s
    static constexpr double kOffset    = -0.691;
    static constexpr double kAbsGate   = -70.0;
    // The floor everything is reported at, rather than negative infinity.
    static constexpr float kSilence    = -70.0f;

    void configure(double sr)
    {
        m_sr = sr > 0.0 ? sr : 48000.0;
        for (int c = 0; c < 2; ++c) {
            m_shelf[c] = k_shelf(m_sr);
            m_hp[c]    = k_highpass(m_sr);
        }
        m_subLen = (int)std::lround(m_sr * kSubMs / 1000.0);
        if (m_subLen < 1) m_subLen = 1;
        reset_all();
    }

    // Everything, including the filters. For a stream that has just started.
    void reset_all()
    {
        for (int c = 0; c < 2; ++c) { m_shelf[c].reset(); m_hp[c].reset(); }
        std::memset(m_sub, 0, sizeof(m_sub));
        m_subN = 0;
        m_filled = 0;
        m_head = 0;
        m_acc[0] = m_acc[1] = 0.0;
        reset_integrated();
        m_short = kSilence;
    }

    // Just the integrated figure, which is the one with a "start again" button:
    // it is a measurement of a take, not a level.
    void reset_integrated()
    {
        std::memset(m_hist, 0, sizeof(m_hist));
        m_blocks = 0;
        m_int = kSilence;
        // A gating block is 400 ms wide, so for the first four sub-blocks after
        // a reset it still contains audio from before it. Counting those would
        // let a loud take leak into the measurement of the next one. Short-term
        // is deliberately left running: it is a live meter, not a measurement.
        m_sinceReset = 0;
    }

    // Interleaved stereo.
    void process(const float* in, int n)
    {
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                const double y = m_hp[c].process(m_shelf[c].process((double)in[i * 2 + c]));
                m_acc[c] += y * y;
            }
            if (++m_subN >= m_subLen) close_sub();
        }
    }

    float short_term() const { return m_short; }
    float integrated() const { return m_int; }

private:
    // Loudness of a mean square per channel. Both channels weigh 1.0; the
    // surround weights in the standard apply to channels this mixer's buses do
    // not have.
    static double loudness_of(double zL, double zR)
    {
        const double z = zL + zR;
        if (!(z > 0.0)) return -1000.0;
        return kOffset + 10.0 * std::log10(z);
    }

    void close_sub()
    {
        m_sub[m_head][0] = m_acc[0];
        m_sub[m_head][1] = m_acc[1];
        m_acc[0] = m_acc[1] = 0.0;
        m_head = (m_head + 1) % kShortSubs;
        if (m_filled < kShortSubs) ++m_filled;
        m_subN = 0;

        // Short term: the last three seconds, once there are three seconds.
        if (m_filled >= kShortSubs) {
            double zL = 0, zR = 0;
            for (int k = 0; k < kShortSubs; ++k) { zL += m_sub[k][0]; zR += m_sub[k][1]; }
            const double f = (double)(kShortSubs * m_subLen);
            const double v = loudness_of(zL / f, zR / f);
            m_short = v < kSilence ? kSilence : (float)v;
        }

        // Integrated: one gating block per sub-block, 400 ms long.
        if (m_sinceReset < (1 << 30)) ++m_sinceReset;
        if (m_filled >= kGateSubs && m_sinceReset >= kGateSubs) {
            double zL = 0, zR = 0;
            for (int k = 1; k <= kGateSubs; ++k) {
                const int idx = (m_head - k + kShortSubs) % kShortSubs;
                zL += m_sub[idx][0]; zR += m_sub[idx][1];
            }
            const double f = (double)(kGateSubs * m_subLen);
            const double v = loudness_of(zL / f, zR / f);
            if (v >= kAbsGate) {
                int bin = (int)std::lround((v - kAbsGate) / kBinLU);
                if (bin < 0) bin = 0;
                if (bin >= kBins) bin = kBins - 1;
                ++m_hist[bin];
                ++m_blocks;
                update_integrated();
            }
        }
    }

    // The relative gate needs the mean of every block that passed the absolute
    // one, so the blocks have to be kept. A histogram at 0.1 LU keeps them in
    // 800 counters instead of a growing list, which matters for a meter that
    // may be left running for hours.
    void update_integrated()
    {
        if (m_blocks == 0) { m_int = kSilence; return; }

        // Mean of the absolutely-gated blocks, in the linear domain.
        double sum = 0.0;
        long long n = 0;
        for (int b = 0; b < kBins; ++b) {
            if (!m_hist[b]) continue;
            sum += m_hist[b] * std::pow(10.0, (bin_lufs(b) - kOffset) / 10.0);
            n   += m_hist[b];
        }
        if (n == 0) { m_int = kSilence; return; }
        const double relGate = kOffset + 10.0 * std::log10(sum / (double)n) - 10.0;

        // And again, over the blocks that also clear the relative gate.
        sum = 0.0; n = 0;
        for (int b = 0; b < kBins; ++b) {
            if (!m_hist[b] || bin_lufs(b) <= relGate) continue;
            sum += m_hist[b] * std::pow(10.0, (bin_lufs(b) - kOffset) / 10.0);
            n   += m_hist[b];
        }
        if (n == 0) { m_int = kSilence; return; }
        const double v = kOffset + 10.0 * std::log10(sum / (double)n);
        m_int = v < kSilence ? kSilence : (float)v;
    }

    static constexpr double kBinLU = 0.1;
    static constexpr int    kBins  = 800;      // -70 .. +10 LUFS
    static double bin_lufs(int b) { return kAbsGate + b * kBinLU; }

    double     m_sr = 48000.0;
    LoudBiquad m_shelf[2], m_hp[2];
    int        m_subLen = 4800, m_subN = 0;
    double     m_acc[2] = {0, 0};
    double     m_sub[kShortSubs][2] = {};
    int        m_head = 0, m_filled = 0;
    int        m_hist[kBins] = {};
    int        m_sinceReset = 0;
    long long  m_blocks = 0;
    float      m_short = kSilence, m_int = kSilence;
};

} // namespace bb
