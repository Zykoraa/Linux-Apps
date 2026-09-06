// Time alignment: the delay line, and the arithmetic that decides how much
// delay each output needs so they all arrive together.
#include "../engine/delay.h"
#include "../engine/click.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
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

    // --- what the listener actually gets ----------------------------------
    {
        near(arrival_ms(32.0f, 234.0f), 266.0, 1e-4, "arrival is the two numbers added");
        near(arrival_ms(266.0f, 0.0f), 266.0, 1e-4, "with no delay it is just the device");
        chk(arrival_ms(-1.0f, 100.0f) < 0.0f,
            "an unreported device has no arrival time, not a 100 ms one");
        near(arrival_ms(10.0f, -5.0f), 10.0, 1e-4, "a negative delay is not subtracted");
    }
    {
        // The real machine: a USB interface at 32 ms and Galaxy Buds at 266.
        const float lat[3] = { 32.0f, 266.0f, 20.0f };
        float del[3] = { 0.0f, 0.0f, 0.0f };
        const bool all[3] = { true, true, true };
        int early = -9, late = -9;
        near(arrival_spread_ms(lat, del, 3, all, &early, &late), 246.0, 1e-4,
             "the spread is between the earliest and the latest, not the first two");
        chk(early == 2 && late == 1, "and it names which two");

        // Aligning has to close it.
        chk(align_delays(lat, del, 3, all), "aligns");
        near(arrival_spread_ms(lat, del, 3, all), 0.0, 1e-3,
             "after aligning there is nothing left to close");
    }
    {
        const float lat[2] = { 32.0f, -1.0f };
        float del[2] = { 0.0f, 0.0f };
        chk(arrival_spread_ms(lat, del, 2) < 0.0f,
            "one usable output has nothing to disagree with, so there is no spread");
    }
    {
        // Excluding the slowest changes the answer - this is the screen-share
        // sink case that made the include mask necessary in the first place.
        const float lat[3] = { 32.0f, 266.0f, 20.0f };
        float del[3] = { 0.0f, 0.0f, 0.0f };
        const bool inc[3] = { true, false, true };
        near(arrival_spread_ms(lat, del, 3, inc), 12.0, 1e-4,
             "an unticked output is not part of the spread");
    }
    {
        // A delay already set counts towards the arrival, so an aligned pair
        // reads as aligned rather than as its raw latencies.
        const float lat[2] = { 32.0f, 266.0f };
        float del[2] = { 234.0f, 0.0f };
        near(arrival_spread_ms(lat, del, 2), 0.0, 1e-4,
             "the spread is between arrivals, not between device latencies");
    }
    {
        chk(gap_verdict(0.0f)   == kGapTogether, "no gap is no gap");
        chk(gap_verdict(0.4f)   == kGapTogether, "under a millisecond fuses");
        chk(gap_verdict(5.0f)   == kGapColour,   "a few ms combs rather than echoes");
        chk(gap_verdict(29.0f)  == kGapColour,   "still one event just under 30 ms");
        chk(gap_verdict(45.0f)  == kGapSlap,     "40-something ms is a slap");
        chk(gap_verdict(234.0f) == kGapEcho,     "a Bluetooth headset against a DAC "
                                                 "is two distinct sounds");
        chk(gap_verdict(-3.0f)  == kGapTogether, "a negative gap is not an echo");
    }

    // --- a delay nothing justifies any more --------------------------------
    {
        // The real one, from the machine this was built on: aligned against
        // Bluetooth earbuds at 303 ms, then the earbuds were unplugged and
        // everything moved to the interface. The 282 ms stayed behind, and
        // every sound was a quarter of a second late with nothing on screen
        // saying why.
        const float lat[3] = { 16.0f, -1.0f, 0.0f };
        const float del[3] = { 282.0f, 0.0f, 0.0f };
        const bool  inc[3] = { true, true, false };
        float want[3];
        near(excess_delay_ms(lat, del, 3, inc, want), 282.0, 1e-4,
             "a delay left over from a device that is gone is all excess");
        near(want[0], 0.0, 1e-4, "and what alignment wants today is nothing");
    }
    {
        // A genuine alignment is not an excess.
        const float lat[3] = { 16.0f, 303.0f, 0.0f };
        const float del[3] = { 287.0f, 0.0f, 0.0f };
        const bool  inc[3] = { true, true, false };
        float want[3];
        near(excess_delay_ms(lat, del, 3, inc, want), 0.0, 1e-4,
             "a delay that meets the slowest device is not excess");
        near(want[0], 287.0, 1e-4, "and it is exactly what alignment wants");
    }
    {
        // Half the story: the device is still there but has become quicker,
        // which is what a Bluetooth codec change does.
        const float lat[2] = { 16.0f, 200.0f };
        const float del[2] = { 287.0f, 0.0f };
        const bool  inc[2] = { true, true };
        float want[2];
        near(excess_delay_ms(lat, del, 2, inc, want), 103.0, 1e-4,
             "a device that got quicker leaves the difference as excess");
    }
    {
        // An unassigned bus carrying a delay is not making anything late, so it
        // is not worth complaining about.
        const float lat[2] = { -1.0f, 20.0f };
        const float del[2] = { 400.0f, 0.0f };
        const bool  inc[2] = { true, true };
        float want[2];
        near(excess_delay_ms(lat, del, 2, inc, want), 0.0, 1e-4,
             "a bus with no reported device is left alone");
        near(want[0], 400.0, 1e-4, "and its delay is reported back unchanged");
    }
    {
        // Excluded outputs are nobody's problem either.
        const float lat[2] = { 16.0f, 20.0f };
        const float del[2] = { 300.0f, 0.0f };
        const bool  inc[2] = { false, true };
        float want[2];
        near(excess_delay_ms(lat, del, 2, inc, want), 0.0, 1e-4,
             "an unticked output is not counted as late");
    }
    {
        // Nothing known at all: no claim either way.
        const float lat[2] = { -1.0f, -1.0f };
        const float del[2] = { 100.0f, 100.0f };
        const bool  inc[2] = { true, true };
        float want[2];
        near(excess_delay_ms(lat, del, 2, inc, want), 0.0, 1e-4,
             "with no latencies known it accuses nothing");
    }

    // --- taking an alignment back -----------------------------------------
    {
        const float before[3] = { 0.0f, 0.0f, 0.0f };
        const float after [3] = { 287.0f, 0.0f, 0.0f };
        chk(align_undo_available(before, after, after, 3),
            "straight after an align, undo is on offer");

        const float nudged[3] = { 290.0f, 0.0f, 0.0f };
        chk(!align_undo_available(before, after, nudged, 3),
            "a delay moved by hand since means undo would restore the wrong thing");

        const float rounding[3] = { 287.02f, 0.0f, 0.0f };
        chk(align_undo_available(before, after, rounding, 3),
            "but a hundredth of a millisecond is float noise, not an edit");

        const float same[3] = { 0.0f, 0.0f, 0.0f };
        chk(!align_undo_available(same, same, same, 3),
            "an align that changed nothing has nothing to undo");

        // Undoing back to a non-zero state is the whole reason this is not
        // simply a "set everything to zero" button.
        const float had[3]  = { 40.0f, 0.0f, 0.0f };
        const float set[3]  = { 287.0f, 0.0f, 0.0f };
        chk(align_undo_available(had, set, set, 3),
            "an align over an existing delay can be taken back to it");
    }

    // --- the timing-test click --------------------------------------------
    {
        ClickTrain c;
        c.configure(kSr);
        near(c.period(), kClickPeriodMs * 0.001f * kSr, 1.0,
             "the period is what the protocol says");
        chk(c.period() > (int)(Delay::kMaxMs * 0.001f * kSr),
            "and is longer than the delay line, so a full delay cannot alias one "
            "tick onto the next");
        chk(!c.running(), "starts silent");

        std::vector<float> buf(512 * 2, 0.0f);
        c.mix(buf.data(), 512);
        chk(buf[0] == 0.0f && buf[1000] == 0.0f, "and adds nothing while it is off");
    }
    {
        // One tick per period, in the same place on every bus.
        ClickTrain c;
        c.configure(kSr);
        c.set_running(true);
        const int block = 256;
        const int total = c.period() * 2 + block;
        std::vector<float> a(block * 2), b(block * 2);
        int ticks = 0, firstAt = -1, maxAt = -1;
        float peak = 0.0f;
        bool sameOnBoth = true, stereo = true;
        for (int off = 0; off < total; off += block) {
            std::fill(a.begin(), a.end(), 0.0f);
            std::fill(b.begin(), b.end(), 0.0f);
            c.mix(a.data(), block);
            c.mix(b.data(), block);          // a second bus, same cycle
            bool inThis = false;
            for (int i = 0; i < block; ++i) {
                if (a[i * 2] != b[i * 2]) sameOnBoth = false;
                if (a[i * 2] != a[i * 2 + 1]) stereo = false;
                const float v = std::fabs(a[i * 2]);
                if (v > peak) { peak = v; maxAt = off + i; }
                if (v > 1e-6f) {
                    inThis = true;
                    if (firstAt < 0) firstAt = off + i;
                }
            }
            if (inThis) ++ticks;
            c.advance(block);
        }
        chk(sameOnBoth, "every bus gets the identical tick from the shared phase");
        chk(stereo, "and it is centred, not on one side");
        chk(firstAt >= 0 && firstAt < c.burst(),
            "the first tick lands immediately, not half a period later");
        chk(ticks >= 2, "it repeats");
        near(peak, ClickTrain::kPeak, ClickTrain::kPeak * 0.05,
             "at the level it advertises");
        chk(maxAt > 0 && maxAt < c.burst(),
            "the loudest sample is inside the burst, in its windowed middle");
    }
    {
        // The tick has to be an even multiple of nothing in particular, but it
        // does have to start and end at silence - a rectangular gate would
        // click at both edges, which is the last thing a test for double
        // arrivals needs.
        ClickTrain c;
        c.configure(kSr);
        c.set_running(true);
        std::vector<float> buf(2048 * 2, 0.0f);
        c.mix(buf.data(), 2048);
        near(buf[0], 0.0, 1e-6, "the burst starts at zero");
        near(buf[(size_t)(c.burst() - 1) * 2], 0.0, 2e-3, "and ends there");
        for (int i = c.burst(); i < 2048; ++i)
            if (buf[(size_t)i * 2] != 0.0f) { chk(false, "silence after the burst"); break; }
    }
    {
        // Restarting begins the train again rather than resuming a free-running
        // phase, so pressing the button always ticks now.
        ClickTrain c;
        c.configure(kSr);
        c.set_running(true);
        c.advance(1000);
        chk(c.phase() == 1000, "the phase runs while it is on");
        c.set_running(false);
        c.advance(1000);
        chk(c.phase() == 0, "and is parked while it is off");
        c.set_running(true);
        chk(c.phase() == 0, "so a restart begins on a tick");
    }
    {
        // The delay line has to carry the tick, or the test would be measuring a
        // path the audio does not take. Measured as the difference between two
        // runs rather than against zero: the tick fades in from silence, so
        // whichever threshold spots it is a few samples late in both, and the
        // subtraction removes that instead of hiding it in a tolerance.
        auto first_tick = [&](float delayMs) {
            ClickTrain c;
            c.configure(kSr);
            c.set_running(true);
            Delay d;
            d.configure(kSr);
            d.set_ms(delayMs);
            const int block = 512, frames = 24000;
            std::vector<float> buf((size_t)block * 2);
            for (int off = 0; off < frames; off += block) {
                std::fill(buf.begin(), buf.end(), 0.0f);
                c.mix(buf.data(), block);
                c.advance(block);
                d.process(buf.data(), block);
                for (int i = 0; i < block; ++i)
                    if (std::fabs(buf[(size_t)i * 2]) > 1e-4f) return off + i;
            }
            return -1;
        };
        const int dry = first_tick(0.0f), wet = first_tick(100.0f);
        chk(dry >= 0 && wet >= 0, "the tick comes out of the delay line");
        near(wet - dry, 100.0 * 0.001 * kSr, 1.0,
             "a hundred milliseconds later, which is what the test is for");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
