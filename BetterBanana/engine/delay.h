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

// --- what the listener actually gets --------------------------------------
//
// The table used to show a device's latency and the delay set against it as two
// separate numbers, and left the addition to the reader. The sum is the whole
// point: it is when the sound reaches you, it is the number that has to match
// across outputs, and it is the number to type into a video player.
inline float arrival_ms(float latency_ms, float delay_ms)
{
    if (!(latency_ms >= 0.0f)) return -1.0f;
    return latency_ms + (delay_ms > 0.0f ? delay_ms : 0.0f);
}

// The spread between the earliest and latest thing you can hear, which is what
// says whether aligning has anything left to do. Returns -1 when fewer than two
// outputs are usable - one output is always in perfect agreement with itself.
// `early`/`late` name which two, so the verdict can say so.
inline float arrival_spread_ms(const float* latency_ms, const float* delay_ms, int n,
                               const bool* include = nullptr,
                               int* early = nullptr, int* late = nullptr)
{
    int lo = -1, hi = -1, usable = 0;
    float loV = 0.0f, hiV = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (include && !include[i]) continue;
        const float a = arrival_ms(latency_ms[i], delay_ms[i]);
        if (a < 0.0f) continue;
        ++usable;
        if (lo < 0 || a < loV) { lo = i; loV = a; }
        if (hi < 0 || a > hiV) { hi = i; hiV = a; }
    }
    if (early) *early = lo;
    if (late)  *late  = hi;
    // Two entries with the same arrival are a spread of zero, which is the
    // answer this whole dialog is trying to reach - not a missing one. Count
    // them rather than asking whether the earliest and latest are the same row.
    if (usable < 2) return -1.0f;
    return hiV - loV;
}

// How much of each output's delay nothing currently justifies.
//
// This is the failure that aligning creates and never cleans up. The delay is
// written to meet the slowest device in the set; unplug that device - swap
// Bluetooth earbuds for wired headphones - and the padding stays behind on
// every other output. Everything is then a quarter of a second late for a
// reason nothing on screen explains, and the mixer looks perfectly healthy
// while it happens.
//
// `want_out` must have room for n, and comes back holding the delay alignment
// would write today. The return value is the worst excess over that.
//
// A bus whose device has not reported a latency is left alone: it is either
// unassigned or unknown, and in neither case is it making anything late.
inline float excess_delay_ms(const float* latency_ms, const float* delay_ms, int n,
                             const bool* include, float* want_out)
{
    for (int i = 0; i < n; ++i) want_out[i] = delay_ms[i];
    align_delays(latency_ms, want_out, n, include);

    float worst = 0.0f;
    for (int i = 0; i < n; ++i) {
        if ((include && !include[i]) || latency_ms[i] < 0.0f) {
            want_out[i] = delay_ms[i];          // nothing to say about this one
            continue;
        }
        const float ex = delay_ms[i] - want_out[i];
        if (ex > worst) worst = ex;
    }
    return worst;
}

// Whether "undo the alignment" is still an honest thing for a button to say.
//
// Two conditions, and both matter. The delays have to be exactly what the align
// wrote, because the moment somebody nudges one by hand the button would be
// promising to restore a state that is no longer the one it replaced. And the
// align has to have actually changed something, or "undo" would do nothing and
// look broken.
inline bool align_undo_available(const float* before, const float* after,
                                 const float* now, int n)
{
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        if (std::fabs(now[i] - after[i]) > 0.05f) return false;
        if (std::fabs(after[i] - before[i]) > 0.05f) changed = true;
    }
    return changed;
}

// What a gap between two arrivals sounds like. The millisecond figure on its
// own means nothing to most people - 30 ms and 300 ms are both "a bit" - and
// the four bands below are where the ear's behaviour actually changes.
enum GapVerdict {
    kGapTogether = 0,   // one sound, and no clue there were ever two
    kGapColour,         // still one sound, but hollow: two copies comb-filter
    kGapSlap,           // a thickening or slap, on the edge of separating
    kGapEcho            // two distinct sounds
};

// Fusion is gradual, not switched, so these are the middles of the transitions
// rather than anything the ear does sharply. Under a millisecond two arrivals
// are a phase difference; up to about 30 ms the precedence effect still fuses
// them into one event while comb filtering colours it; past 50 ms speech starts
// to separate, and by 60 ms almost anyone hears two.
inline GapVerdict gap_verdict(float gap_ms)
{
    const float g = gap_ms < 0.0f ? 0.0f : gap_ms;
    if (g < 1.0f)  return kGapTogether;
    if (g < 30.0f) return kGapColour;
    if (g < 60.0f) return kGapSlap;
    return kGapEcho;
}

inline const char* gap_sounds_like(GapVerdict v)
{
    switch (v) {
    case kGapTogether: return "one sound";
    case kGapColour:   return "one sound, but hollow and phasey";
    case kGapSlap:     return "a slap, on the edge of splitting in two";
    default:           return "two distinct sounds - an echo";
    }
}

} // namespace bb
