// The voice changer. The pitch shifter is checked by measuring what comes out
// with the analyser's own FFT rather than by trusting the arithmetic.
#include "../engine/voicefx.h"
#include "../engine/spectrum.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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
        std::printf("  FAIL  %s: got %.3f want %.3f (+-%.3f)\n", what, got, want, tol);
    }
}

// Dominant frequency of a block, by peak FFT bin with parabolic interpolation.
static double dominant_hz(const float* x, int n, double sr)
{
    std::vector<float> re(x, x + n), im(n, 0.0f);
    // Hann, so the peak is not smeared across the whole spectrum by the edges.
    for (int i = 0; i < n; ++i)
        re[i] *= 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (n - 1)));
    fft(re.data(), im.data(), n);
    int best = 1;
    double bestMag = 0.0;
    for (int k = 1; k < n / 2; ++k) {
        const double m = std::hypot(re[k], im[k]);
        if (m > bestMag) { bestMag = m; best = k; }
    }
    const double a = std::hypot(re[best - 1], im[best - 1]);
    const double b = std::hypot(re[best],     im[best]);
    const double c = std::hypot(re[best + 1], im[best + 1]);
    const double denom = a - 2 * b + c;
    const double off = std::fabs(denom) < 1e-12 ? 0.0 : 0.5 * (a - c) / denom;
    return (best + off) * sr / n;
}

// Runs a mono chain, discarding `warm` samples so the delay lines have filled.
template <typename F>
static std::vector<float> run(F&& step, double freq, double sr, int warm, int keep)
{
    std::vector<float> out;
    out.reserve(keep);
    for (int i = 0; i < warm + keep; ++i) {
        const float x = (float)std::sin(2.0 * M_PI * freq * i / sr);
        const float y = step(x);
        if (i >= warm) out.push_back(y);
    }
    return out;
}

int main()
{
    std::printf("test_voicefx\n");
    const double SR = 48000.0;

    // --- the pitch shifter --------------------------------------------------
    {
        // Zero semitones must be a literal pass-through: the two heads sit at
        // fixed offsets there, and summing them would be a comb filter.
        PitchShifter ps;
        ps.set_semitones(0.0f);
        bool same = true;
        for (int i = 0; i < 4096; ++i) {
            const float x = (float)std::sin(2.0 * M_PI * 440.0 * i / SR);
            if (ps.process(x) != x) { same = false; break; }
        }
        chk(same, "zero semitones is bit-identical, not a comb filter");
    }
    {
        struct Case { float st; double want; const char* what; };
        const Case cases[] = {
            { 12.0f,  880.0, "+12 semitones doubles the pitch" },
            {  7.0f,  659.3, "+7 semitones is a fifth up" },
            { -12.0f, 220.0, "-12 semitones halves the pitch" },
            { -5.0f,  329.6, "-5 semitones is a fourth down" },
        };
        for (const Case& cs : cases) {
            PitchShifter ps;
            ps.set_semitones(cs.st);
            auto out = run([&](float x) { return ps.process(x); }, 440.0, SR, 16384, 8192);
            const double f = dominant_hz(out.data(), 8192, SR);
            // A granular shifter puts sidebands a grain rate either side; the
            // dominant partial should still be the shifted fundamental.
            near(f, cs.want, cs.want * 0.02, cs.what);
        }
    }
    {
        // It must not fall apart into silence or blow up.
        PitchShifter ps;
        ps.set_semitones(-9.0f);
        auto out = run([&](float x) { return ps.process(x); }, 200.0, SR, 16384, 8192);
        float peak = 0.0f;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        chk(peak > 0.4f && peak < 1.6f, "level stays sane through a -9 semitone shift");
    }

    // --- ring modulator -----------------------------------------------------
    {
        RingMod rm;
        rm.configure((float)SR, 100.0f, 1.0f);
        auto out = run([&](float x) { return rm.process(x); }, 1000.0, SR, 1024, 8192);
        // Full-depth ring modulation replaces the carrier with two sidebands at
        // 900 and 1100 Hz; the 1000 Hz original should be gone.
        std::vector<float> re(out.begin(), out.end()), im(8192, 0.0f);
        for (int i = 0; i < 8192; ++i)
            re[i] *= 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / 8191));
        fft(re.data(), im.data(), 8192);
        auto mag = [&](double hz) {
            const int k = (int)std::lround(hz * 8192 / SR);
            return std::hypot(re[k], im[k]);
        };
        chk(mag(900.0) > 10.0 * mag(1000.0), "ring mod: lower sideband replaces the carrier");
        chk(mag(1100.0) > 10.0 * mag(1000.0), "ring mod: upper sideband too");

        RingMod off;
        off.configure((float)SR, 0.0f, 1.0f);
        chk(off.process(0.37f) == 0.37f, "ring mod at 0 Hz is off");
    }

    // --- crusher ------------------------------------------------------------
    {
        Crusher c;
        c.bits = 4; c.down = 1;
        // 4 bits -> 8 levels either side of zero, so a step of 0.125.
        const float y = c.process(0.31f);
        near(std::fmod(std::fabs(y), 0.125f), 0.0, 1e-5, "bit crush quantises to its step");
        chk(std::fabs(y - 0.31f) < 0.125f, "and stays near the input");

        Crusher d;
        d.bits = 0; d.down = 4;
        const float a = d.process(1.0f);
        const float b = d.process(-1.0f);
        chk(a == 1.0f && b == 1.0f, "downsampling holds the sample it grabbed");

        Crusher off;
        chk(off.process(0.37f) == 0.37f, "a default crusher is transparent");
    }

    // --- drive --------------------------------------------------------------
    {
        near(drive_shape(0.0f, 5.0f), 0.0, 1e-6, "drive leaves silence alone");
        near(drive_shape(1.0f, 5.0f), 1.0, 1e-5, "drive is normalised at full scale");
        chk(drive_shape(0.5f, 8.0f) > 0.5f, "drive lifts what is below full scale");
        chk(drive_shape(0.5f, 8.0f) < 1.0f, "and does not exceed it");
    }

    // --- echo ---------------------------------------------------------------
    {
        Echo e;
        e.configure((float)SR, 10.0f, 0.0f, 1.0f);      // 480 samples, no feedback
        float first = e.process(1.0f);
        near(first, 1.0, 1e-6, "echo passes the dry signal straight through");
        // 479 more calls puts the write head exactly one delay past the sample,
        // so the call after that is the one that reads it back.
        for (int i = 0; i < 479; ++i) e.process(0.0f);
        near(e.process(0.0f), 1.0, 1e-6, "and repeats it one delay later");

        Echo off;
        chk(off.process(0.37f) == 0.37f, "a default echo is transparent");
    }

    // --- the whole rack -----------------------------------------------------
    {
        auto shm = std::make_unique<Shared>();
        set_defaults(shm.get());
        VoiceFxChain fx;
        fx.configure((float)SR);
        fx.update(shm->strip[0].fx, (float)SR);          // all defaults
        bool same = true;
        for (int i = 0; i < 4096; ++i) {
            const float x = (float)std::sin(2.0 * M_PI * 300.0 * i / SR);
            if (std::fabs(fx.process(0, x) - x) > 1e-6f) { same = false; break; }
        }
        chk(same, "a default rack is transparent end to end");

        // And that the chain actually engages when asked.
        shm->strip[0].fx.pitch.store(12.0f);
        fx.update(shm->strip[0].fx, (float)SR);
        std::vector<float> out;
        for (int i = 0; i < 24576; ++i) {
            const float x = (float)std::sin(2.0 * M_PI * 440.0 * i / SR);
            const float y = fx.process(0, x);
            if (i >= 16384) out.push_back(y);
        }
        near(dominant_hz(out.data(), 8192, SR), 880.0, 20.0,
             "the rack shifts pitch when the block asks for it");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
