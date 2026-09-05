// Loudness, ITU-R BS.1770-4 and EBU Tech 3341.
//
// The point of the compliance case below is that it validates everything at
// once - the two K-weighting filters, the channel summing, the -0.691 offset
// and the gating - against a number published by someone else. Any of those
// wrong on its own moves the answer.
#include "../engine/loudness.h"

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
        std::printf("  FAIL  %s: got %.4f want %.4f (tol %.4f)\n", what, got, want, tol);
    }
}

// A steady stereo sine of the given PEAK amplitude, fed through the meter.
static void feed_sine(Loudness& m, double sr, double hz, double amp, double seconds)
{
    const int n = (int)(sr * seconds);
    std::vector<float> buf(512 * 2);
    int done = 0;
    while (done < n) {
        const int m2 = (n - done) > 512 ? 512 : (n - done);
        for (int i = 0; i < m2; ++i) {
            const float v = float(amp * std::sin(2.0 * M_PI * hz * (done + i) / sr));
            buf[(size_t)i * 2] = v; buf[(size_t)i * 2 + 1] = v;
        }
        m.process(buf.data(), m2);
        done += m2;
    }
}

static void feed_silence(Loudness& m, double sr, double seconds)
{
    const int n = (int)(sr * seconds);
    std::vector<float> buf(512 * 2, 0.0f);
    int done = 0;
    while (done < n) {
        const int m2 = (n - done) > 512 ? 512 : (n - done);
        m.process(buf.data(), m2);
        done += m2;
    }
}

int main()
{
    std::printf("test_loudness\n");

    // --- the filters are derived, so check the derivation ---------------------
    // BS.1770-4 publishes these coefficients for 48 kHz. Deriving them from the
    // analog specification is what makes the meter correct at 44.1 and 96 kHz
    // too, but it is only worth anything if it reproduces the published set.
    {
        const LoudBiquad s = k_shelf(48000.0);
        near(s.b0,  1.53512485958697, 1e-9, "shelf b0 matches the published value");
        near(s.b1, -2.69169618940638, 1e-9, "shelf b1 matches");
        near(s.b2,  1.19839281085285, 1e-9, "shelf b2 matches");
        near(s.a1, -1.69065929318241, 1e-9, "shelf a1 matches");
        near(s.a2,  0.73248077421585, 1e-9, "shelf a2 matches");

        const LoudBiquad h = k_highpass(48000.0);
        near(h.b0,  1.0, 1e-9, "high-pass b0 matches");
        near(h.b1, -2.0, 1e-9, "high-pass b1 matches");
        near(h.b2,  1.0, 1e-9, "high-pass b2 matches");
        near(h.a1, -1.99004745483398, 1e-8, "high-pass a1 matches");
        near(h.a2,  0.99007225036621, 1e-8, "high-pass a2 matches");
    }

    // --- EBU Tech 3341 case 1 -------------------------------------------------
    // A 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS. Note where the 0.69 dB
    // comes from: the arithmetic alone gives -23.69, and the K-weighting's gain
    // at 1 kHz supplies the rest. Getting either piece wrong shows up here.
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        near(m.integrated(), -23.0, 0.1, "EBU 3341-1: -23 dBFS sine reads -23.0 LUFS");
        near(m.short_term(), -23.0, 0.1, "and short-term agrees");
    }

    // --- and the scale is a scale --------------------------------------------
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -33.0 / 20.0), 10.0);
        near(m.integrated(), -33.0, 0.1, "ten dB quieter reads ten LUFS lower");
    }

    // --- silence is reported as silence, not as zero -------------------------
    {
        Loudness m; m.configure(48000.0);
        feed_silence(m, 48000.0, 5.0);
        near(m.integrated(), Loudness::kSilence, 1e-6, "silence integrates to the floor");
        near(m.short_term(), Loudness::kSilence, 1e-6, "and reads the floor short-term");
    }

    // --- gating: quiet passages must not drag the number down ----------------
    // This is the whole reason the integrated figure is gated. Without it, the
    // pauses between sentences would make a mix read quieter the longer nobody
    // spoke, and it would never settle.
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        const float loudOnly = m.integrated();
        feed_silence(m, 48000.0, 30.0);
        near(m.integrated(), loudOnly, 0.1,
             "thirty seconds of silence does not move the integrated figure");
        near(m.short_term(), Loudness::kSilence, 1e-6,
             "while short-term drops to the floor, as it should");
    }

    // --- the relative gate drops the quiet half ------------------------------
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        // 20 LU below is past the -10 LU relative gate, so it is excluded.
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -43.0 / 20.0), 10.0);
        near(m.integrated(), -23.0, 0.2,
             "a passage 20 LU down is gated out of the integrated figure");
    }

    // --- and it is not simply ignoring everything quiet ----------------------
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        // 6 LU down clears the relative gate, so it must pull the mean down.
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -29.0 / 20.0), 10.0);
        const double v = m.integrated();
        chk(v < -23.5 && v > -29.0,
            "a passage 6 LU down is inside the gate and does move it");
    }

    // --- rate independence ---------------------------------------------------
    // The reason the coefficients are derived rather than pasted in.
    {
        Loudness a; a.configure(44100.0);
        feed_sine(a, 44100.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        near(a.integrated(), -23.0, 0.15, "44.1 kHz reads the same as 48");

        Loudness b; b.configure(96000.0);
        feed_sine(b, 96000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 10.0);
        near(b.integrated(), -23.0, 0.15, "and so does 96 kHz");
    }

    // --- resetting the take, but not the meter -------------------------------
    {
        Loudness m; m.configure(48000.0);
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -13.0 / 20.0), 6.0);
        near(m.integrated(), -13.0, 0.1, "a loud take reads loud");
        m.reset_integrated();
        near(m.integrated(), Loudness::kSilence, 1e-6, "resetting clears it");
        feed_sine(m, 48000.0, 1000.0, std::pow(10.0, -23.0 / 20.0), 6.0);
        near(m.integrated(), -23.0, 0.1, "and the next take starts from nothing");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
