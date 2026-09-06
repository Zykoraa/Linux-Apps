// betterbanana - the timing-test click.
//
// Everything else in the alignment path is a number: PipeWire says a device
// costs 266 ms, the mixer holds another one back by 234, and the arithmetic
// agrees. None of that tells you whether it is right, and the reported figure
// for a Bluetooth sink is the one figure most likely to be a little off - the
// codec is negotiated with the headset, not declared by it.
//
// So there is something to listen to. A tick, on whichever outputs you choose,
// from one shared phase: if they arrive together you hear one tick, and if they
// do not you hear a flam. Turning the delay while it plays walks the flam
// closed, which is a thing anybody can do and no amount of reading a table is.
//
// Realtime-safe by construction: the burst is a table computed once at
// configure(), and the mixer does an add and a compare per sample.
#pragma once

#include <cmath>

#include "../common/protocol.h"

namespace bb {

class ClickTrain {
public:
    // Short and bright. A tick survives SBC and AAC as a transient; a thud
    // arrives smeared, which is exactly the thing being measured.
    static constexpr float kBurstMs  = 3.0f;
    static constexpr float kToneHz   = 2000.0f;
    static constexpr float kPeak     = 0.10f;    // -20 dBFS, well under a mix
    static constexpr int   kMaxBurst = 1024;     // 3 ms at 192 kHz, with slack

    void configure(float sr)
    {
        m_sr = sr > 0.0f ? sr : 48000.0f;
        m_period = (int)std::lround(kClickPeriodMs * 0.001f * m_sr);
        if (m_period < 2) m_period = 2;
        m_burst = (int)std::lround(kBurstMs * 0.001f * m_sr);
        if (m_burst > kMaxBurst) m_burst = kMaxBurst;
        if (m_burst > m_period)  m_burst = m_period;

        const double kTwoPi = 6.283185307179586;
        for (int i = 0; i < m_burst; ++i) {
            // Hann, so the tick starts and ends at zero. A rectangular gate on
            // a 2 kHz tone clicks twice - at both edges - which is the last
            // thing a test for double arrivals needs.
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * (double)i / (double)m_burst));
            m_tab[i] = (float)(kPeak * w * std::sin(kTwoPi * kToneHz * (double)i / (double)m_sr));
        }
        m_phase = 0;
    }

    // Set once per graph cycle from the shared mask, before any bus mixes.
    void set_running(bool on)
    {
        // Start from the first tick rather than wherever the free-running phase
        // sat, so pressing the button ticks now instead of in half a second.
        if (on && !m_on) m_phase = 0;
        m_on = on;
    }

    bool running()  const { return m_on; }
    int  period()   const { return m_period; }
    int  burst()    const { return m_burst; }
    int  phase()    const { return m_phase; }

    // Add the tick into one interleaved stereo block. Every bus is mixed from
    // the same phase, so the tick leaves all of them on the same sample and
    // whatever separation you hear is the thing under test and nothing else.
    void mix(float* io, uint32_t n) const
    {
        if (!m_on || m_burst <= 0) return;
        for (uint32_t i = 0; i < n; ++i) {
            int p = m_phase + (int)i;
            while (p >= m_period) p -= m_period;
            if (p >= m_burst) continue;
            io[i * 2]     += m_tab[p];
            io[i * 2 + 1] += m_tab[p];
        }
    }

    // Once per cycle, after every bus has mixed from it.
    void advance(uint32_t n)
    {
        if (!m_on) { m_phase = 0; return; }
        long long p = (long long)m_phase + (long long)n;
        m_phase = (int)(p % (long long)m_period);
    }

private:
    float m_tab[kMaxBurst] = {};
    float m_sr = 48000.0f;
    int   m_period = 1, m_burst = 0, m_phase = 0;
    bool  m_on = false;
};

} // namespace bb
