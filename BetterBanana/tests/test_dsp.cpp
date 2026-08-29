#include "../engine/dsp.h"
#include <cstdio>
#include <vector>
using namespace bb;

static int fails = 0;
static void chk(bool ok, const char* what, double got, double want, double tol)
{
    if (!ok) { std::printf("  FAIL  %-46s got %10.4f want %10.4f (tol %g)\n", what, got, want, tol); ++fails; }
    else       std::printf("  ok    %-46s %10.4f\n", what, got);
}
static void near(double got, double want, double tol, const char* what)
{ chk(std::fabs(got - want) <= tol, what, got, want, tol); }

int main()
{
    const float sr = 48000.0f;
    std::printf("\n[biquad response]\n");
    {
        Biquad b;
        b.set_peaking(sr, 1000.0f, 1.0f, 6.0f);
        near(b.magnitude_db(sr, 1000.0f), 6.0, 0.01, "peaking +6dB @1k -> gain at centre");
        near(b.magnitude_db(sr, 50.0f),   0.0, 0.30, "peaking +6dB @1k -> flat far below");
        b.set_peaking(sr, 1000.0f, 1.0f, -9.0f);
        near(b.magnitude_db(sr, 1000.0f), -9.0, 0.01, "peaking -9dB @1k -> cut at centre");

        b.set_lowshelf(sr, 200.0f, 0.707f, 8.0f);
        near(b.magnitude_db(sr, 10.0f),   8.0, 0.15, "lowshelf +8dB -> full boost at DC end");
        near(b.magnitude_db(sr, 200.0f),  4.0, 0.40, "lowshelf +8dB -> half boost at corner");

        b.set_highshelf(sr, 6000.0f, 0.707f, -5.0f);
        near(b.magnitude_db(sr, 20000.0f), -5.0, 0.30, "highshelf -5dB -> full cut at top");

        b.set_peaking(sr, 1000.0f, 1.0f, 0.0f);
        near(b.magnitude_db(sr, 1000.0f), 0.0, 1e-6, "0 dB gain -> exact bypass");
    }

    std::printf("\n[biquad stability: 60s of noise, no blowup]\n");
    {
        Biquad b; b.set_peaking(sr, 40.0f, 8.0f, 12.0f);
        float mx = 0; unsigned seed = 1;
        for (int i = 0; i < 48000 * 60; ++i) {
            seed = seed * 1103515245u + 12345u;
            float x = ((seed >> 9) & 0x7fffff) / 4194304.0f - 1.0f;
            float y = b.process(x);
            if (std::fabs(y) > mx) mx = std::fabs(y);
        }
        chk(std::isfinite(mx) && mx < 40.0f, "high-Q low-freq boost stays bounded", mx, 0, 40);
    }

    std::printf("\n[gate]\n");
    {
        Gate g; g.configure(sr); g.set_knob(5.0f);   // threshold -40 dB
        float out = 0;
        for (int i = 0; i < 24000; ++i) out = g.process(0.5f * std::sin(i * 0.05f));
        chk(g.gain > 0.95f, "loud signal -> gate open", g.gain, 1.0, 0.05);
        for (int i = 0; i < 48000; ++i) out = g.process(0.0001f * std::sin(i * 0.05f));
        chk(g.gain < 0.05f, "quiet signal -> gate closed", g.gain, 0.0, 0.05);
        (void)out;
        Gate off; off.configure(sr); off.set_knob(0.0f);
        near(off.process(0.123f), (double)0.123f, 0.0, "knob 0 -> bit-exact passthrough");
        // Turning the gate off must clear its reported reduction, or the GUI
        // keeps drawing a closed gate forever.
        g.set_knob(0.0f);
        near(g.gain, 1.0, 0.0, "disabling after closing reports no reduction");
    }

    std::printf("\n[compressor]\n");
    {
        auto peak_of = [&](float knob, float amp) {
            Compressor c; c.configure(sr); c.set_knob(knob);
            float mx = 0;
            for (int i = 0; i < 48000; ++i) {
                float y = c.process(amp * std::sin(2.0f * kPi * 220.0f * i / sr));
                if (i > 24000 && std::fabs(y) > mx) mx = std::fabs(y);
            }
            return lin_to_db(mx);
        };
        const float loud_in = lin_to_db(1.0f), quiet_in = lin_to_db(0.1f);
        float loud_out = peak_of(6.0f, 1.0f), quiet_out = peak_of(6.0f, 0.1f);
        float range_in  = loud_in - quiet_in;
        float range_out = loud_out - quiet_out;
        chk(range_out < range_in - 3.0f, "knob 6 -> dynamic range compressed", range_out, range_in, 0);
        std::printf("        (in range %.1f dB -> out range %.1f dB)\n", range_in, range_out);

        Compressor off; off.configure(sr); off.set_knob(0.0f);
        near(off.process(0.321f), (double)0.321f, 0.0, "knob 0 -> bit-exact passthrough");
        Compressor c2; c2.configure(sr); c2.set_knob(8.0f);
        for (int i = 0; i < 24000; ++i) c2.process(0.9f * std::sin(i * 0.05f));
        chk(c2.gr_db < -0.5f, "compressing reports gain reduction", c2.gr_db, -1, 0);
        c2.set_knob(0.0f);
        near(c2.gr_db, 0.0, 0.0, "disabling clears reported gain reduction");
    }

    std::printf("\n[pan law]\n");
    {
        float l, r;
        pan_gains(0.0f, l, r);
        near(l, r, 1e-6, "centre -> equal L/R");
        near(l * l + r * r, 1.0, 1e-5, "centre -> constant power");
        pan_gains(-1.0f, l, r);
        near(r, 0.0, 1e-6, "hard left -> R silent");
        near(l, 1.0, 1e-5, "hard left -> L unity");
        pan_gains(0.35f, l, r);
        near(l * l + r * r, 1.0, 1e-5, "arbitrary pos -> constant power");
    }

    std::printf("\n[meter ballistics]\n");
    {
        PeakMeter m; m.configure(sr);
        std::vector<float> blk(480, 0.8f);
        m.feed(blk.data(), blk.size());
        near(m.peak, 0.8, 1e-6, "reads block peak instantly");
        std::vector<float> sil(480, 0.0f);
        for (int i = 0; i < 10; ++i) m.feed(sil.data(), sil.size());   // 100 ms
        chk(m.peak < 0.8f && m.peak > 0.1f, "decays gradually, not instantly", m.peak, 0.0, 0);
        chk(m.hold > 0.79f, "peak-hold survives the decay window", m.hold, 0.8, 0.01);
    }

    std::printf("\n[db/lin round trip]\n");
    near(db_to_lin(0.0f), 1.0, 1e-9, "0 dB -> 1.0");
    near(db_to_lin(-6.0206f), 0.5, 1e-4, "-6.02 dB -> 0.5");
    near(lin_to_db(db_to_lin(-23.7f)), -23.7, 1e-3, "round trip");
    near(db_to_lin(-200.0f), 0.0, 1e-12, "below floor -> hard zero");

    std::printf("\n%s (%d failure%s)\n\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
