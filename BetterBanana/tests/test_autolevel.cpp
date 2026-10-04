// Auto-level: does it reach the target, stay inside its limits, and - just as
// important - leave alone what it should leave alone: silence, a noise floor,
// gaps between bursts, a bus the ducker is working on, a bus it is switched
// off for.
//
// Everything is measured with the engine's own BS.1770 meter, so "-16 LUFS"
// here means what the loudness column in the mixer shows.
#include "../engine/autolevel.h"
#include "../engine/loudness.h"

#include <cmath>
#include <cstdint>
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
        std::printf("  FAIL  %s: got %.3f want %.3f (+-%.3f)\n", what, got, want, tol);
    }
}

// Deterministic white noise, stereo, scaled so the engine's meter reads `lufs`.
struct Noise {
    uint32_t state = 12345;
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return ((state >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f;
    }
};

static std::vector<float> noise(double sr, double secs, double lufs, uint32_t seed = 12345)
{
    const int n = (int)(sr * secs);
    // Generate at least 3 s so the reference measurement fills the meter's
    // short-term window even for a one-second burst, then keep the first n.
    const int gen = std::max(n, (int)(sr * 3.0));
    std::vector<float> v(gen * 2);
    Noise ns; ns.state = seed;
    for (auto& x : v) x = ns.next();
    Loudness m; m.configure(sr);
    m.process(v.data(), (int)(sr * 3.0));
    v.resize(n * 2);
    const double got = m.short_term();
    const float k = (float)std::pow(10.0, (lufs - got) / 20.0);
    for (auto& x : v) x *= k;
    return v;
}

static double lufs_of(const float* io, int frames, double sr)
{
    Loudness m; m.configure(sr);
    m.process(io, frames);
    return m.short_term();
}

static AutoLevel make(double sr, bool on = true)
{
    AutoLevel a;
    a.configure(sr);
    a.set(kAlDefaultTarget, kAlDefaultBoost, kAlDefaultCut);
    a.set_enabled(on);
    return a;
}

int main()
{
    const double sr = 48000.0;
    const int s3 = (int)(3 * sr);

    // --- reaches the target from below and above ---------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        near(lufs_of(v.data() + v.size() - s3 * 2, s3, sr), -16.0, 0.5,
             "a quiet source is brought up to -16 LUFS within 20 s");
        near(a.gain_db(), 14.0, 0.6, "by about the 14 dB it was short");
        chk(a.state() == kAlActive, "and says it is tracking");
    }
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -8.0);
        a.process(v.data(), (int)(v.size() / 2));
        near(lufs_of(v.data() + v.size() - s3 * 2, s3, sr), -16.0, 0.5,
             "a loud source is brought down to -16 LUFS");
    }

    // --- stays inside its limits -------------------------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 30.0, -40.0);
        a.process(v.data(), (int)(v.size() / 2));
        near(a.gain_db(), kAlDefaultBoost, 0.05, "boost stops at its limit");
        near(lufs_of(v.data() + v.size() - s3 * 2, s3, sr), -22.0, 0.5,
             "so -40 LUFS comes out at -22, not -16");
        chk(a.state() == kAlAtLimit, "and says it is at its limit");
    }
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -1.0);
        a.process(v.data(), (int)(v.size() / 2));
        near(a.gain_db(), -kAlDefaultCut, 0.05, "cut stops at its limit");
    }

    // --- refuses to boost a noise floor ------------------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -50.0);
        a.process(v.data(), (int)(v.size() / 2));
        near(a.gain_db(), 0.0, 1e-6, "a -50 LUFS hiss is left at 0 dB, not lifted by 18");
        chk(a.state() == kAlHeldQuiet, "and it says it is holding");
    }

    // --- holds through silence ---------------------------------------------
    // Programme, then thirty seconds of nothing: the gain must still be what
    // the programme needed when it comes back, not have crept up or decayed.
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        const float before = a.gain_db();
        std::vector<float> quiet((size_t)(30 * sr) * 2, 0.0f);
        a.process(quiet.data(), (int)(quiet.size() / 2));
        near(a.gain_db(), before, 0.01, "thirty seconds of silence leave the gain alone");
        chk(a.state() == kAlHeldQuiet, "while it holds");
    }

    // --- gaps do not make it creep -----------------------------------------
    // One second on, one second off. An ungated meter would read 3 dB low and
    // the rider would chase it upward; the gated one sees the bursts as they are.
    {
        AutoLevel a = make(sr);
        auto on = noise(sr, 1.0, -30.0, 99);
        std::vector<float> off(on.size(), 0.0f);
        for (int k = 0; k < 40; ++k) {
            std::vector<float> b = on;
            a.process(b.data(), (int)(b.size() / 2));
            std::vector<float> z = off;
            a.process(z.data(), (int)(z.size() / 2));
        }
        chk(a.gain_db() < 14.6f, "bursts with gaps are not boosted past what they need");
        chk(a.gain_db() > 13.0f, "but are brought up");
    }

    // --- moves slowly, and smoothly ----------------------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 12.0, -36.0);
        const int sub = (int)(sr / 10);
        float worst_rise = 0.0f, last = a.gain_db();
        float worst_step = 0.0f, gprev = 1.0f;
        for (size_t f = 0; f < v.size() / 2; ++f) {
            const float g = a.frame(v[f * 2], v[f * 2 + 1]);
            worst_step = std::max(worst_step, std::fabs(g - gprev));
            gprev = g;
            if ((f + 1) % (size_t)sub == 0) {
                worst_rise = std::max(worst_rise, a.gain_db() - last);
                last = a.gain_db();
            }
        }
        chk(worst_rise <= AutoLevel::kRiseDbPerS * 0.1f + 1e-4f,
            "it never rises faster than its slew limit");
        chk(worst_step < 2e-4f, "and the gain has no step a listener could hear as a click");
    }
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        auto loud = noise(sr, 1.0, -6.0, 7);
        const float before = a.gain_db();
        a.process(loud.data(), (int)(loud.size() / 2));
        chk(before - a.gain_db() <= AutoLevel::kFallDbPerS * 1.0f + 0.01f,
            "a sudden loud source is cut at no more than its fall rate");
        chk(before - a.gain_db() > 1.0f, "but it is cut");
    }

    // --- holds for the ducker ----------------------------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        const float before = a.gain_db();
        a.set_hold(true);
        auto ducked = noise(sr, 6.0, -42.0, 5);
        a.process(ducked.data(), (int)(ducked.size() / 2));
        near(a.gain_db(), before, 0.01, "while held it does not ride ducked music back up");
        chk(a.state() == kAlHeldDuck, "and says why");
    }

    // --- off means off -----------------------------------------------------
    {
        AutoLevel a = make(sr);
        auto v = noise(sr, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        a.set_enabled(false);
        std::vector<float> s((size_t)(15 * sr) * 2, 0.1f);
        a.process(s.data(), (int)(s.size() / 2));
        near(a.gain_db(), 0.0, 1e-6, "switched off it ramps back to unity");
        chk(!a.live(), "and then drops out of the signal path");
        auto w = noise(sr, 1.0, -20.0, 3);
        std::vector<float> in = w;
        a.process(w.data(), (int)(w.size() / 2));
        bool same = true;
        for (size_t k = 0; k < w.size(); ++k) same = same && w[k] == in[k];
        chk(same, "so its output is the input, bit for bit");
        chk(a.state() == kAlOff, "and it says it is off");
    }
    {
        AutoLevel a = make(sr, false);
        chk(!a.live(), "a stage that was never switched on is not in the path at all");
    }

    // --- the same at other sample rates ------------------------------------
    for (double r : { 44100.0, 96000.0 }) {
        AutoLevel a = make(r);
        auto v = noise(r, 20.0, -30.0);
        a.process(v.data(), (int)(v.size() / 2));
        const int t3 = (int)(3 * r);
        near(lufs_of(v.data() + v.size() - t3 * 2, t3, r), -16.0, 0.5,
             r < 48000 ? "and at 44.1 kHz" : "and at 96 kHz");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
