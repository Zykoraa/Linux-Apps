// betterbanana - bus modes.
//
// A bus sums its strips in stereo. A mode decides what comes out of the other
// side and how many channels that takes, so a bus can drive a 5.1 card, feed a
// subwoofer only, or send a mono-safe mix to something that will be played back
// on one speaker.
//
// The matrices here are BetterBanana's own. Voicemeeter's are not published,
// and inventing behaviour to fit a borrowed name is worse than defining one:
// "UpMix 4.1" below means exactly what this file says it means. The two modes
// of the original that describe mixing one bus into another (Amix, Bmix) have
// no counterpart in this architecture and are deliberately absent rather than
// present and wrong.
//
// Header-only and free of PipeWire, so tests/test_surround.cpp can drive it.
#pragma once

#include "../common/protocol.h"

#include <cmath>
#include <cstring>

namespace bb {

// The channels a bus can publish, named rather than numbered. engine.cpp maps
// these to SPA_AUDIO_CHANNEL_*; nothing else needs to know about SPA.
enum BusChanId : int {
    kChFL = 0, kChFR, kChFC, kChLFE, kChBL, kChBR, kChSL, kChSR
};

struct BusLayout {
    int         channels;
    int         chan[8];        // BusChanId per output channel, in WAVE order
    const char* name;           // what the mode is called in the UI and bb-ctl
    const char* help;           // one line, shown as a tooltip
};

// WAVE/PipeWire channel order is FL, FR, FC, LFE, BL, BR, SL, SR, and a layout
// that omits one simply closes the gap - 4.1 is FL, FR, LFE, BL, BR.
inline const BusLayout& bus_layout(int mode)
{
    static const BusLayout kLayouts[] = {
        { 2, { kChFL, kChFR }, "Normal",
          "Stereo, straight through" },
        { 2, { kChFL, kChFR }, "TV mix",
          "Stereo narrowed to 35% width, so it survives being played in mono" },
        { 4, { kChFL, kChFR, kChBL, kChBR }, "Repeat",
          "Front pair copied to the rear pair, unchanged" },
        { 3, { kChFL, kChFR, kChLFE }, "Up-mix 2.1",
          "Stereo plus a crossed-over subwoofer channel" },
        { 5, { kChFL, kChFR, kChLFE, kChBL, kChBR }, "Up-mix 4.1",
          "Fronts, subwoofer, and rears carrying the stereo difference" },
        { 6, { kChFL, kChFR, kChFC, kChLFE, kChBL, kChBR }, "Up-mix 5.1",
          "Centre taken out of the fronts, subwoofer crossed over, rears decorrelated" },
        { 8, { kChFL, kChFR, kChFC, kChLFE, kChBL, kChBR, kChSL, kChSR }, "Up-mix 7.1",
          "5.1 with the sides carrying a second, longer-delayed rear image" },
        { 6, { kChFL, kChFR, kChFC, kChLFE, kChBL, kChBR }, "Centre only",
          "A 5.1 stream with signal on the centre channel alone" },
        { 6, { kChFL, kChFR, kChFC, kChLFE, kChBL, kChBR }, "Subwoofer only",
          "A 5.1 stream with signal on the LFE channel alone" },
        { 6, { kChFL, kChFR, kChFC, kChLFE, kChBL, kChBR }, "Rears only",
          "A 5.1 stream with signal on the rear pair alone" },
    };
    static_assert(sizeof(kLayouts) / sizeof(kLayouts[0]) == (size_t)kBusModeCount,
                  "the layout table and BusMode in protocol.h have drifted apart");
    if (mode < 0 || mode >= kBusModeCount) mode = kBusNormal;
    return kLayouts[mode];
}

// Stereo in, whatever the mode asks for out.
//
// Two pieces of state, both per bus and both touched only on the audio thread:
// the crossover that splits bass off to the LFE channel, and the delay that
// keeps the rears from fusing with the fronts into one image in the middle of
// the room.
class Upmix {
public:
    // 120 Hz is where a small satellite stops being able to help.
    static constexpr float kCrossHz  = 120.0f;
    // Long enough for the precedence effect to keep the image at the front,
    // short enough not to read as an echo.
    static constexpr float kRearMs   = 15.0f;
    static constexpr float kSideMs   = 24.0f;
    static constexpr int   kMaxDelay = 8192;    // 24 ms at 192 kHz, with room

    void configure(float sr)
    {
        m_sr = sr > 0.0f ? sr : 48000.0f;
        // One-pole coefficient; two of them in series make the 12 dB/octave
        // Linkwitz-Riley pair the test checks for flat summing.
        m_a = 1.0f - std::exp(-2.0f * 3.14159265358979f * kCrossHz / m_sr);
        m_rearD = clampDelay(int(kRearMs * 0.001f * m_sr));
        m_sideD = clampDelay(int(kSideMs * 0.001f * m_sr));
        reset();
    }

    void reset()
    {
        m_lp1 = m_lp2 = m_hpL1 = m_hpL2 = m_hpR1 = m_hpR2 = 0.0f;
        std::memset(m_dl, 0, sizeof(m_dl));
        m_w = 0;
    }

    // `in` is interleaved stereo, `n` frames. `out` is interleaved with
    // bus_layout(mode).channels. The two must not overlap.
    void process(int mode, const float* in, float* out, int n)
    {
        const BusLayout& L = bus_layout(mode);
        const int nc = L.channels;

        for (int f = 0; f < n; ++f) {
            const float l = in[f * 2], r = in[f * 2 + 1];
            const float mid = 0.5f * (l + r);
            const float side = 0.5f * (l - r);

            float ch[8] = {};

            switch (mode) {
            case kBusNormal:                                     // Normal
                ch[kChFL] = l; ch[kChFR] = r;
                break;

            case kBusTvMix: {                                   // TV mix
                const float w = 0.35f;
                ch[kChFL] = mid + w * side;
                ch[kChFR] = mid - w * side;
                break;
            }
            case kBusRepeat:                                     // Repeat
                ch[kChFL] = l; ch[kChFR] = r;
                ch[kChBL] = l; ch[kChBR] = r;
                break;

            case kBusUpMix21: {                                   // 2.1
                ch[kChLFE] = lfe(mid);
                ch[kChFL] = hpL(l);
                ch[kChFR] = hpR(r);
                break;
            }
            case kBusUpMix41: {                                   // 4.1
                ch[kChLFE] = lfe(mid);
                ch[kChFL] = hpL(l);
                ch[kChFR] = hpR(r);
                const float rear = delayed(m_rearD) * kRearGain;
                ch[kChBL] =  rear;
                ch[kChBR] = -rear;
                break;
            }
            case kBusUpMix51: {                                   // 5.1
                fivePointOne(l, r, mid, ch);
                break;
            }
            case kBusUpMix71: {                                   // 7.1
                fivePointOne(l, r, mid, ch);
                const float sideCh = delayed(m_sideD) * kRearGain;
                ch[kChSL] =  sideCh;
                ch[kChSR] = -sideCh;
                break;
            }
            case kBusCenterOnly:                                     // Centre only
                ch[kChFC] = mid;
                break;
            case kBusLfeOnly:                                     // Subwoofer only
                ch[kChLFE] = lfe(mid);
                break;
            case kBusRearOnly: {                                   // Rears only
                const float rear = delayed(m_rearD) * kRearGain;
                ch[kChBL] =  rear;
                ch[kChBR] = -rear;
                break;
            }
            default:
                ch[kChFL] = l; ch[kChFR] = r;
                break;
            }

            // Advance the delay line once per frame regardless of the mode, so
            // switching modes does not leave a stale line to be read later.
            push(side);

            for (int c = 0; c < nc; ++c) out[f * nc + c] = ch[L.chan[c]];
        }
    }

private:
    // Half the centre is taken OUT of the fronts and put into the centre
    // channel, so the sum is still L and R and the phantom centre does not end
    // up doubled once a real centre speaker is playing it too.
    static constexpr float kCentre   = 0.5f;
    static constexpr float kRearGain = 0.7f;

    void fivePointOne(float l, float r, float mid, float* ch)
    {
        ch[kChFC]  = kCentre * mid;
        ch[kChLFE] = lfe(mid);
        ch[kChFL]  = hpL(l - kCentre * mid);
        ch[kChFR]  = hpR(r - kCentre * mid);
        const float rear = delayed(m_rearD) * kRearGain;
        ch[kChBL] =  rear;
        ch[kChBR] = -rear;
    }

    // Two cascaded one-poles: 12 dB/octave, and the inversion is what makes the
    // low and high halves sum back to flat. tests/test_surround.cpp measures it.
    float lfe(float x)
    {
        m_lp1 += m_a * (x - m_lp1);
        m_lp2 += m_a * (m_lp1 - m_lp2);
        return -m_lp2;
    }
    float hpL(float x)
    {
        m_hpL1 += m_a * (x - m_hpL1);
        const float h1 = x - m_hpL1;
        m_hpL2 += m_a * (h1 - m_hpL2);
        return h1 - m_hpL2;
    }
    float hpR(float x)
    {
        m_hpR1 += m_a * (x - m_hpR1);
        const float h1 = x - m_hpR1;
        m_hpR2 += m_a * (h1 - m_hpR2);
        return h1 - m_hpR2;
    }

    static int clampDelay(int d) { return d < 1 ? 1 : (d > kMaxDelay - 1 ? kMaxDelay - 1 : d); }
    void  push(float v) { m_dl[m_w] = v; m_w = (m_w + 1) % kMaxDelay; }
    // Read d frames back. push() has not run for this frame yet, so m_w is the
    // slot the current sample is about to occupy.
    float delayed(int d) const { return m_dl[(m_w + kMaxDelay - d) % kMaxDelay]; }

    float m_sr = 48000.0f, m_a = 0.0f;
    float m_lp1 = 0, m_lp2 = 0, m_hpL1 = 0, m_hpL2 = 0, m_hpR1 = 0, m_hpR2 = 0;
    float m_dl[kMaxDelay] = {};
    int   m_w = 0, m_rearD = 1, m_sideD = 1;
};

} // namespace bb
