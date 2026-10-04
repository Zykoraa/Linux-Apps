// betterbanana - auto-level: a slow loudness rider for a bus.
//
// Meant for the stream bus. Whatever the applications feeding it are set to -
// a video player left at 9%, a game that is louder than the music - the people
// watching hear it at a steady, normal loudness, so nobody has to balance their
// own headphones against what a stream needs.
//
// Feed-forward: it measures what comes INTO the stage and sets the gain from
// that, never from its own output, so there is no loop through the limiter
// and nothing to go unstable. The measurement is BS.1770 loudness (the same
// K-weighting as engine/loudness.h) over a sliding 3 s window, built from
// 100 ms sub-blocks; only sub-blocks louder than kSubGate count, so the pauses
// between sentences or songs do not drag the reading down and make it creep
// the gain up.
//
// It refuses to guess. With less than a second of real material in the window,
// or a reading below kHoldBelow (a noise floor, not programme), it HOLDS the
// gain it has rather than boosting hiss. It also holds while the ducker is
// working on the bus: ducked music is quieter on purpose, and riding it back up
// would undo the ducker within seconds.
//
// The gain moves at most kRiseDbPerS up and kFallDbPerS down - slow enough not
// to pump with the music, quicker to come down than up because a sudden loud
// source matters more than a quiet one. Each 100 ms step is interpolated
// sample by sample so there is no zipper.
#pragma once

#include "dsp.h"
#include "loudness.h"
#include "../common/protocol.h"

#include <cmath>

namespace bb {

class AutoLevel {
public:
    static constexpr int    kSubMs      = 100;
    static constexpr int    kWinSubs    = 30;       // 3 s, the short-term window
    static constexpr int    kMinLive    = 10;       // 1 s of material before it acts
    static constexpr double kSubGate    = -50.0;    // LUFS; quieter sub-blocks are pauses
    static constexpr double kHoldBelow  = -45.0;    // LUFS; below this it is noise
    static constexpr float  kRiseDbPerS = 1.5f;
    static constexpr float  kFallDbPerS = 4.0f;

    void configure(double sr)
    {
        m_sr = sr > 0.0 ? sr : 48000.0;
        for (int c = 0; c < 2; ++c) { m_shelf[c] = k_shelf(m_sr); m_hp[c] = k_highpass(m_sr); }
        m_subLen = (int)std::lround(m_sr * kSubMs / 1000.0);
        if (m_subLen < 1) m_subLen = 1;
        // 150 ms, in double: a float one-pole this slow stalls about 4e-4 short
        // of its target, because each step falls below float resolution near
        // 1.0 - harmless as level, but "off" must land on exactly unity.
        m_coeff = std::exp(-1.0 / (0.150 * m_sr));
        m_cur = m_tgt = 1.0;
        m_gain_db = 0.0f;
        reset_analysis();
    }

    void set(float target, float max_boost, float max_cut)
    {
        m_target = target;
        m_boost = max_boost < 0.0f ? 0.0f : max_boost;
        m_cut   = max_cut   < 0.0f ? 0.0f : max_cut;
    }

    void set_hold(bool h) { m_hold = h; }

    // Off ramps back to 0 dB at the normal rate and then bypasses entirely.
    // Coming back on starts a fresh measurement: the window may describe
    // whatever was playing minutes ago.
    void set_enabled(bool on)
    {
        if (on && !m_on) reset_analysis();
        m_on = on;
    }

    // True while the stage does anything at all. When it is off and has
    // settled at unity the caller can skip it, and its output is then the
    // input exactly.
    bool live() const { return m_on || m_gain_db != 0.0f || m_cur != 1.0; }

    // One stereo frame in; the linear gain to apply to that frame out.
    inline float frame(float L, float R)
    {
        if (m_on) {
            const double yl = m_hp[0].process(m_shelf[0].process((double)L));
            const double yr = m_hp[1].process(m_shelf[1].process((double)R));
            m_acc += yl * yl + yr * yr;
        }
        if (++m_subN >= m_subLen) {
            sub_block(m_acc / m_subLen);
            m_acc = 0.0;
            m_subN = 0;
        }
        m_cur = m_tgt + (m_cur - m_tgt) * m_coeff;
        // Close enough to land exactly, so "off" becomes bit-transparent.
        if (!m_on && m_gain_db == 0.0f && std::fabs(m_cur - 1.0) < 1e-9) m_cur = 1.0;
        return (float)m_cur;
    }

    // Block form, interleaved stereo, in place. Used by the tests.
    void process(float* io, int n)
    {
        for (int i = 0; i < n; ++i) {
            const float g = frame(io[i * 2], io[i * 2 + 1]);
            io[i * 2] *= g; io[i * 2 + 1] *= g;
        }
    }

    float gain_db()  const { return m_gain_db; }
    int   state()    const { return m_state; }
    float measured() const { return m_measured; }

private:
    void reset_analysis()
    {
        for (int c = 0; c < 2; ++c) { m_shelf[c].reset(); m_hp[c].reset(); }
        for (int k = 0; k < kWinSubs; ++k) m_sub[k] = 0.0;
        m_head = 0; m_filled = 0; m_acc = 0.0; m_subN = 0;
        m_measured = (float)Loudness::kSilence;
    }

    static double to_lufs(double ms) { return ms > 0.0 ? Loudness::kOffset + 10.0 * std::log10(ms) : -200.0; }

    void sub_block(double ms)
    {
        const float dt = kSubMs / 1000.0f;
        float want = m_gain_db;

        if (!m_on) {
            want = 0.0f;
            m_state = kAlOff;
        } else {
            m_sub[m_head] = ms;
            m_head = (m_head + 1) % kWinSubs;
            if (m_filled < kWinSubs) ++m_filled;

            // Gated mean over the window: pauses are left out, not averaged in.
            double e = 0.0;
            int live = 0;
            for (int k = 0; k < m_filled; ++k)
                if (to_lufs(m_sub[k]) > kSubGate) { e += m_sub[k]; ++live; }
            const double lufs = live ? to_lufs(e / live) : -200.0;
            m_measured = lufs < Loudness::kSilence ? Loudness::kSilence : (float)lufs;

            if (m_hold) {
                m_state = kAlHeldDuck;
            } else if (live < kMinLive || lufs < kHoldBelow) {
                m_state = kAlHeldQuiet;
            } else {
                const float need = m_target - (float)lufs;
                want = need > m_boost ? m_boost : need < -m_cut ? -m_cut : need;
                m_state = (need > m_boost + 0.05f || need < -m_cut - 0.05f) ? kAlAtLimit : kAlActive;
            }
        }

        if (want > m_gain_db) m_gain_db = std::fmin(want, m_gain_db + kRiseDbPerS * dt);
        else if (want < m_gain_db) m_gain_db = std::fmax(want, m_gain_db - kFallDbPerS * dt);
        m_tgt = std::pow(10.0, m_gain_db / 20.0);
    }

    double m_sr = 48000.0;
    LoudBiquad m_shelf[2], m_hp[2];
    double m_sub[kWinSubs] = {};
    int    m_head = 0, m_filled = 0, m_subN = 0, m_subLen = 4800;
    double m_acc = 0.0;

    double m_cur = 1.0, m_tgt = 1.0, m_coeff = 0.0;
    float m_gain_db = 0.0f;
    float m_target = kAlDefaultTarget, m_boost = kAlDefaultBoost, m_cut = kAlDefaultCut;
    float m_measured = (float)Loudness::kSilence;
    bool  m_on = false, m_hold = false;
    int   m_state = kAlOff;
};

} // namespace bb
