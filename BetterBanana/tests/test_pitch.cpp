// The pitch detector behind the voice calibration wizard. Checked against
// synthetic voices at known fundamentals, including the two ways this kind of
// detector normally fails: reporting an octave low on a rich harmonic series,
// and reporting confident nonsense on something that is not a voice at all.
#include "../engine/pitchtrack.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        std::printf("  FAIL  %s: got %.2f want %.2f (+-%.2f)\n", what, got, want, tol);
    }
}

// A voice-shaped signal: harmonics of f0 under two formant bumps.
static std::vector<float> voice(double f0, double sr, double secs, double jitter = 0.0)
{
    auto env = [](double f) {
        auto bump = [&](double c, double w) {
            const double t = std::log(f / c) / w;
            return std::exp(-0.5 * t * t);
        };
        return bump(700.0, 0.30) + 0.7 * bump(1800.0, 0.25) + 0.02;
    };
    const int n = int(sr * secs);
    std::vector<float> x(n, 0.0f);
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        // A little drift, because a real voice is never dead steady.
        const double f = f0 * (1.0 + jitter * std::sin(2.0 * M_PI * 0.7 * i / sr));
        phase += 2.0 * M_PI * f / sr;
        double v = 0.0;
        for (int h = 1; h * f <= 12000.0; ++h) v += env(h * f) * std::sin(h * phase);
        x[i] = float(v * 0.12);
    }
    return x;
}

int main()
{
    std::printf("test_pitch\n");
    const double SR = 48000.0;

    for (double f0 : { 85.0, 110.0, 135.0, 180.0, 220.0, 300.0 }) {
        auto x = voice(f0, SR, 2.0);
        const PitchEstimate e = estimate_pitch(x.data(), (int)x.size(), (float)SR);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "a %.0f Hz voice measures as %.1f Hz", f0, e.median_hz);
        near(e.median_hz, f0, f0 * 0.03, msg);
        std::snprintf(msg, sizeof(msg), "and %.0f Hz reads as voiced throughout", f0);
        chk(e.ok() && e.voiced > e.frames / 2, msg);
    }

    {
        // The classic failure: a strong harmonic series read an octave low.
        auto x = voice(120.0, SR, 2.0);
        const PitchEstimate e = estimate_pitch(x.data(), (int)x.size(), (float)SR);
        chk(e.median_hz > 90.0f, "a rich voice is not reported an octave low");
        chk(e.median_hz < 160.0f, "nor an octave high");
    }
    {
        // A wobbling voice still gives a sensible median and a wider spread.
        auto x = voice(140.0, SR, 3.0, 0.06);
        const PitchEstimate e = estimate_pitch(x.data(), (int)x.size(), (float)SR);
        near(e.median_hz, 140.0, 8.0, "a drifting voice still medians correctly");
        chk(e.high_hz > e.low_hz, "and reports a range around it");
        chk(e.high_hz - e.low_hz > 4.0f, "which is wider than for a steady tone");
    }
    {
        std::vector<float> noise(int(SR * 2));
        unsigned s = 12345;
        for (float& v : noise) { s = s * 1103515245u + 12345u; v = ((s >> 9) % 2000) / 1000.0f - 1.0f; }
        const PitchEstimate e = estimate_pitch(noise.data(), (int)noise.size(), (float)SR);
        chk(!e.ok() || e.voiced < e.frames / 4, "noise is not confidently pitched");
    }
    {
        std::vector<float> silence(int(SR * 2), 0.0f);
        const PitchEstimate e = estimate_pitch(silence.data(), (int)silence.size(), (float)SR);
        chk(e.voiced == 0 && !e.ok(), "silence yields no voiced frames at all");
    }
    {
        // What the wizard actually does with the answer.
        near(semitones_between(120.0f, 200.0f), 8.84, 0.02, "120 -> 200 Hz is 8.8 semitones");
        near(semitones_between(180.0f, 90.0f), -12.0, 0.01, "halving is exactly -12");
        near(semitones_between(0.0f, 200.0f), 0.0, 1e-6, "a missing measurement shifts nothing");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
