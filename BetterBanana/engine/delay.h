// betterbanana - time alignment.
//
// Two output devices almost never have the same latency. A Bluetooth headset
// runs a quarter of a second behind a USB interface, so anything feeding both
// arrives twice, noticeably apart. PipeWire already knows the figure for every
// device - it is reported per node, and for a Bluetooth sink it is the codec
// and link delay that nothing else can see - so aligning them is a matter of
// holding the early ones back, not of measuring anything.
//
// A plain ring, no interpolation: the delay is set from a control, not
// modulated, and a sample of quantisation at 48 kHz is 20 microseconds. The
// length changes only at a block boundary, so a moved fader cannot tear a block
// in half.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bb {

class Delay {
public:
    // Bluetooth is the reason for the ceiling; 500 ms is twice the worst
    // A2DP figure seen in the wild.
    static constexpr float kMaxMs = 500.0f;

    void configure(float sr)
    {
        m_sr = sr > 0.0f ? sr : 48000.0f;
        m_cap = (int)std::lround(kMaxMs * 0.001f * m_sr) + 2;
        if (m_cap > kMaxFrames) m_cap = kMaxFrames;
        reset();
    }

    void reset()
    {
        std::memset(m_buf, 0, sizeof(m_buf));
        m_w = 0;
        m_frames = m_want;
    }

    // Set from the control thread's value once per block, never mid-block.
    void set_ms(float ms)
    {
        if (!(ms > 0.0f)) ms = 0.0f;
        if (ms > kMaxMs) ms = kMaxMs;
        int f = (int)std::lround(ms * 0.001f * m_sr);
        if (f > m_cap - 1) f = m_cap - 1;
        m_want = f;
    }

    bool active() const { return m_frames > 0 || m_want > 0; }

    // Interleaved stereo, in place. `n` frames.
    void process(float* io, int n)
    {
        // Adopt a new length only between blocks. Changing it inside one would
        // read from a part of the ring that has not been written yet.
        m_frames = m_want;
        if (m_frames <= 0) {
            // Still keep the ring fed, so switching the delay back on does not
            // replay whatever was in it the last time it was used.
            for (int i = 0; i < n; ++i) {
                m_buf[m_w * 2] = io[i * 2];
                m_buf[m_w * 2 + 1] = io[i * 2 + 1];
                m_w = m_w + 1 == m_cap ? 0 : m_w + 1;
            }
            return;
        }
        for (int i = 0; i < n; ++i) {
            const int r = (m_w - m_frames + m_cap) % m_cap;
            const float l = m_buf[r * 2], rr = m_buf[r * 2 + 1];
            m_buf[m_w * 2] = io[i * 2];
            m_buf[m_w * 2 + 1] = io[i * 2 + 1];
            io[i * 2] = l; io[i * 2 + 1] = rr;
            m_w = m_w + 1 == m_cap ? 0 : m_w + 1;
        }
    }

private:
    // 500 ms at 192 kHz, stereo.
    static constexpr int kMaxFrames = 96002;

    float m_buf[kMaxFrames * 2] = {};
    float m_sr = 48000.0f;
    int   m_cap = kMaxFrames;
    int   m_w = 0;
    int   m_frames = 0, m_want = 0;
};

// What a set of device latencies implies for alignment: hold everything back to
// meet the slowest one. Returns false when nothing usable is known, so a caller
// can say so rather than silently setting every delay to zero.
//
// `latency_ms` entries below zero mean "not reported"; those are left alone
// rather than treated as instant, which would delay everything else to match a
// device nobody measured.
//
// `include` is what stops this from being actively harmful. Not every output is
// something a person is listening to in the room: a bus feeding a null sink for
// a screen share is heard by people somewhere else entirely, and aligning it
// only adds a quarter of a second to what they hear. Excluded entries neither
// count towards the slowest nor get a delay written.
inline bool align_delays(const float* latency_ms, float* delay_ms_out, int n,
                         const bool* include = nullptr)
{
    auto in = [&](int i) { return !include || include[i]; };

    float slowest = -1.0f;
    for (int i = 0; i < n; ++i)
        if (in(i) && latency_ms[i] >= 0.0f) slowest = std::max(slowest, latency_ms[i]);
    if (slowest < 0.0f) return false;
    for (int i = 0; i < n; ++i)
        if (in(i) && latency_ms[i] >= 0.0f)
            delay_ms_out[i] = std::min(slowest - latency_ms[i], Delay::kMaxMs);
    return true;
}

} // namespace bb
