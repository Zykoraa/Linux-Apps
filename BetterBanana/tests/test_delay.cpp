// Time alignment: the delay line, and the arithmetic that decides how much
// delay each output needs so they all arrive together.
#include "../engine/delay.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static void near(double got, double want, double tol, const char* what)
{
    ++g_total;
    if (std::fabs(got - want) > tol) {
        ++g_fail;
        std::printf("  FAIL  %s: got %.4f want %.4f\n", what, got, want);
    }
}

static constexpr float kSr = 48000.0f;

// Sends one impulse through and reports which frame it comes out on, or -1.
static int impulse_at(float ms, int frames = 40000, int block = 512)
{
    Delay d;
    d.configure(kSr);
    d.set_ms(ms);
    std::vector<float> buf((size_t)block * 2);
    int found = -1;
    for (int off = 0; off < frames; off += block) {
        const int n = (frames - off) > block ? block : (frames - off);
        for (int i = 0; i < n * 2; ++i) buf[(size_t)i] = 0.0f;
        if (off == 0) { buf[0] = 1.0f; buf[1] = 1.0f; }
        d.process(buf.data(), n);
        for (int i = 0; i < n && found < 0; ++i)
            if (std::fabs(buf[(size_t)i * 2]) > 0.5f) found = off + i;
        if (found >= 0) break;
    }
    return found;
}

int main()
{
    std::printf("test_delay\n");

    // --- no delay must be a true pass-through, sample for sample -------------
    {
        Delay d; d.configure(kSr); d.set_ms(0.0f);
        float io[8] = { 0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.75f, 0.1f, -0.9f };
        const float want[8] = { 0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.75f, 0.1f, -0.9f };
        d.process(io, 4);
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && io[i] == want[i];
        chk(same, "zero delay changes nothing");
    }

    // --- and a delay is the length it claims --------------------------------
    for (float ms : { 1.0f, 10.0f, 50.0f, 253.0f }) {
        const int want = (int)std::lround(ms * 0.001f * kSr);
        near(impulse_at(ms), want, 1.0,
             ms == 253.0f ? "253 ms - a Bluetooth headset's worth - lands where it should"
                          : "the impulse comes out after exactly the delay asked for");
    }

    // --- the ceiling holds ---------------------------------------------------
    {
        const int at = impulse_at(5000.0f);          // far past the maximum
        const int cap = (int)std::lround(Delay::kMaxMs * 0.001f * kSr);
        chk(at > 0 && at <= cap, "an absurd delay is clamped rather than wrapping round");
    }

    // --- crossing a block boundary must not tear -----------------------------
    // The length is adopted between blocks, never inside one; a delay that
    // changed mid-block would read from a part of the ring not yet written.
    {
        Delay d; d.configure(kSr);
        d.set_ms(20.0f);
        std::vector<float> buf(256 * 2, 0.0f);
        for (int b = 0; b < 40; ++b) {
            for (int i = 0; i < 256; ++i) {
                const float v = std::sin(2.0f * 3.14159265f * 440.0f * (b * 256 + i) / kSr);
                buf[(size_t)i * 2] = v; buf[(size_t)i * 2 + 1] = v;
            }
            if (b == 20) d.set_ms(35.0f);            // moved mid-stream
            d.process(buf.data(), 256);
            for (int i = 0; i < 512; ++i)
                if (!std::isfinite(buf[(size_t)i]) || std::fabs(buf[(size_t)i]) > 1.5f) {
                    chk(false, "changing the delay mid-stream produced a bad sample");
                    b = 99; break;
                }
        }
        chk(true, "the delay can be moved while audio is running");
    }

    // --- switching off and on again must not replay the old contents ---------
    {
        Delay d; d.configure(kSr);
        d.set_ms(0.0f);
        std::vector<float> buf(512 * 2, 0.0f);
        for (int i = 0; i < 512 * 2; ++i) buf[(size_t)i] = 1.0f;
        d.process(buf.data(), 512);                  // loud, with the delay off
        d.set_ms(5.0f);
        for (int i = 0; i < 512 * 2; ++i) buf[(size_t)i] = 0.0f;
        d.process(buf.data(), 512);                  // silence, delay now on
        double pk = 0.0;
        for (int i = 0; i < 512 * 2; ++i) pk = std::fmax(pk, std::fabs(buf[(size_t)i]));
        chk(pk > 0.5, "the ring keeps running while the delay is off, so turning it "
                      "on continues the signal instead of replaying stale audio");
    }

    // --- alignment: hold the fast ones back to meet the slowest --------------
    // Eve's actual machine: a USB interface at 10.7 ms and Galaxy Buds at 253 ms.
    {
        const float lat[3] = { 10.7f, 253.0f, -1.0f };
        float out[3] = { 9, 9, 9 };
        chk(align_delays(lat, out, 3), "alignment works when at least one is known");
        near(out[0], 242.3, 0.01, "the USB interface is held back to meet the headset");
        near(out[1], 0.0,   1e-6, "the headset, being slowest, is not delayed at all");
        near(out[2], 9.0,   1e-6,
             "a bus with no reported latency keeps whatever delay it had, rather "
             "than being silently zeroed on the strength of a device nobody measured");
    }

    // --- an output nobody is listening to must not drag the rest ------------
    // A bus feeding a screen share is heard somewhere else entirely. Aligning
    // it adds a quarter of a second to what those people hear and helps no one,
    // and if it were the slowest it would hold back the whole room to match it.
    {
        const float lat[3] = { 32.0f, 266.0f, 0.0f };
        const bool  inc[3] = { true, true, false };     // A3 is the stream bus
        float out[3] = { 0, 0, 0 };
        chk(align_delays(lat, out, 3, inc), "alignment runs with an exclusion");
        near(out[0], 234.0, 1e-4, "the USB interface still meets the headset");
        near(out[1], 0.0,   1e-6, "the headset is still the one to meet");
        near(out[2], 0.0,   1e-6, "and the excluded bus is left completely alone");
    }
    {
        // The excluded one being the slowest must not decide the answer either.
        const float lat[3] = { 32.0f, 60.0f, 400.0f };
        const bool  inc[3] = { true, true, false };
        float out[3] = { 0, 0, 0 };
        chk(align_delays(lat, out, 3, inc), "alignment runs");
        near(out[0], 28.0, 1e-4, "the slowest INCLUDED output is what sets the target");
        near(out[1], 0.0,  1e-6, "and it is the one left undelayed");
    }
    {
        const float lat[2] = { 10.0f, 20.0f };
        const bool  inc[2] = { false, false };
        float out[2] = { 5, 5 };
        chk(!align_delays(lat, out, 2, inc),
            "excluding everything reports failure rather than doing nothing quietly");
        chk(out[0] == 5.0f, "and changes nothing");
    }
    {
        const float lat[3] = { -1.0f, -1.0f, -1.0f };
        float out[3] = { 9, 9, 9 };
        chk(!align_delays(lat, out, 3),
            "with nothing known it reports failure rather than zeroing every delay");
        chk(out[0] == 9.0f, "and leaves the delays untouched");
    }
    {
        // A gap wider than the delay line can cover is clamped, not wrapped.
        const float lat[2] = { 0.0f, 900.0f };
        float out[2] = {};
        chk(align_delays(lat, out, 2), "a huge gap still aligns");
        near(out[0], Delay::kMaxMs, 1e-6, "clamped to what the delay line can hold");
        near(out[1], 0.0, 1e-6, "and the slowest is still undelayed");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
