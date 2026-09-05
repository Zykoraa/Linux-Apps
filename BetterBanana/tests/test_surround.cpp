// Bus modes: the layouts, the matrices, and the two bits of DSP that can be
// wrong without sounding obviously wrong - the subwoofer crossover, which has
// to hand back a flat response when its two halves are added up again, and the
// rear delay, which has to be the length it claims.
#include "../engine/surround.h"

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
        std::printf("  FAIL  %s: got %.5f want %.5f\n", what, got, want);
    }
}

static constexpr float kSr = 48000.0f;

// Runs a steady sine through a mode and returns the peak of each output
// channel, measured over the last quarter so the filters have settled.
static std::vector<double> peaks(int mode, double hz, double ampL, double ampR,
                                 int frames = 24000)
{
    Upmix u;
    u.configure(kSr);
    const BusLayout& L = bus_layout(mode);
    std::vector<float> in((size_t)frames * 2), out((size_t)frames * L.channels);
    for (int f = 0; f < frames; ++f) {
        const double s = std::sin(2.0 * M_PI * hz * f / kSr);
        in[(size_t)f * 2]     = float(ampL * s);
        in[(size_t)f * 2 + 1] = float(ampR * s);
    }
    u.process(mode, in.data(), out.data(), frames);

    std::vector<double> pk((size_t)L.channels, 0.0);
    for (int f = frames * 3 / 4; f < frames; ++f)
        for (int c = 0; c < L.channels; ++c)
            pk[(size_t)c] = std::fmax(pk[(size_t)c],
                                      std::fabs((double)out[(size_t)f * L.channels + c]));
    return pk;
}

int main()
{
    std::printf("test_surround\n");

    // --- the layouts are WAVE-ordered and the right width --------------------
    chk(bus_layout(0).channels == 2, "Normal is stereo");
    chk(bus_layout(3).channels == 3, "2.1 is three channels");
    chk(bus_layout(4).channels == 5, "4.1 is five channels");
    chk(bus_layout(5).channels == 6, "5.1 is six channels");
    chk(bus_layout(6).channels == 8, "7.1 is eight channels");
    {
        const BusLayout& L = bus_layout(5);
        chk(L.chan[0] == kChFL && L.chan[1] == kChFR && L.chan[2] == kChFC &&
            L.chan[3] == kChLFE && L.chan[4] == kChBL && L.chan[5] == kChBR,
            "5.1 is in WAVE order: FL FR FC LFE BL BR");
        const BusLayout& F = bus_layout(4);
        chk(F.chan[2] == kChLFE && F.chan[3] == kChBL,
            "4.1 closes the gap where the centre would be");
    }
    chk(bus_layout(-1).channels == 2 && bus_layout(99).channels == 2,
        "an unknown mode falls back to stereo rather than reading off the end");

    // --- Normal is not allowed to touch anything -----------------------------
    {
        Upmix u; u.configure(kSr);
        float in[8] = { 0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.75f, 0.1f, -0.9f };
        float out[8] = {};
        u.process(0, in, out, 4);
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && out[i] == in[i];
        chk(same, "Normal passes stereo through bit for bit");
    }

    // --- Repeat really is a copy ---------------------------------------------
    {
        Upmix u; u.configure(kSr);
        float in[4] = { 0.5f, -0.25f, 1.0f, 0.0f };
        float out[8] = {};
        u.process(2, in, out, 2);
        chk(out[0] == 0.5f && out[1] == -0.25f && out[2] == 0.5f && out[3] == -0.25f,
            "Repeat sends the front pair to the rear pair unchanged");
    }

    // --- TV mix narrows without moving the mono sum --------------------------
    {
        // Hard left, well above the crossover so nothing else is in play.
        const auto p = peaks(1, 1000.0, 1.0, 0.0);
        near(p[0], 0.5 * (1.0 + 0.35), 0.02, "TV mix keeps 67% of a hard-left signal left");
        near(p[1], 0.5 * (1.0 - 0.35), 0.02, "and puts the rest on the right");
        near(p[0] + p[1], 1.0, 0.02, "so the mono sum is unchanged");
    }

    // --- the crossover: each half does its job -------------------------------
    {
        const auto low  = peaks(3, 30.0,   1.0, 1.0);
        const auto high = peaks(3, 4000.0, 1.0, 1.0);
        chk(low[2] > 0.85, "30 Hz reaches the subwoofer channel");
        chk(low[0] < 0.15, "and is kept out of the fronts");
        chk(high[0] > 0.95, "4 kHz reaches the fronts");
        chk(high[2] < 0.05, "and is kept out of the subwoofer");
    }

    // --- and together they add back up to flat -------------------------------
    // This is the one that is easy to get wrong. Two cascaded one-poles make a
    // Linkwitz-Riley pair, and LP + HP of that pair CANCEL at the crossover
    // frequency - the halves only reconstruct with one of them inverted, which
    // is why lfe() hands back a negated signal. Get the polarity backwards and
    // there is a hole an octave wide exactly where the bass lives, in a room
    // where nobody can hear the two halves separately to tell.
    {
        double worst = 0.0, worstHz = 0.0;
        for (double hz : { 20.0, 40.0, 60.0, 90.0, 120.0, 160.0, 240.0, 400.0, 1000.0 }) {
            Upmix u; u.configure(kSr);
            const int frames = 48000;
            std::vector<float> in((size_t)frames * 2), out((size_t)frames * 3);
            for (int f = 0; f < frames; ++f) {
                const float s = float(std::sin(2.0 * M_PI * hz * f / kSr));
                in[(size_t)f * 2] = in[(size_t)f * 2 + 1] = s;
            }
            u.process(3, in.data(), out.data(), frames);
            double pk = 0.0;
            for (int f = frames * 3 / 4; f < frames; ++f) {
                // What the room does: the front speaker and the subwoofer add.
                // The LFE already carries the inversion the pair needs.
                const double sum = out[(size_t)f * 3] + out[(size_t)f * 3 + 2];
                pk = std::fmax(pk, std::fabs(sum));
            }
            const double db = 20.0 * std::log10(pk > 1e-9 ? pk : 1e-9);
            if (std::fabs(db) > std::fabs(worst)) { worst = db; worstHz = hz; }
        }
        ++g_total;
        if (std::fabs(worst) > 1.0) {
            ++g_fail;
            std::printf("  FAIL  crossover halves do not sum flat: %.2f dB at %.0f Hz\n",
                        worst, worstHz);
        } else {
            std::printf("  crossover sums flat within %.2f dB (worst at %.0f Hz)\n",
                        worst, worstHz);
        }
    }

    // --- 5.1 takes the centre OUT of the fronts ------------------------------
    // Otherwise a real centre speaker plays it as well and the middle of the
    // image comes out 3 dB loud and twice as wide.
    {
        const auto p = peaks(5, 1000.0, 1.0, 1.0);   // dead centre content
        near(p[2], 0.5, 0.02, "half the centre content goes to the centre channel");
        near(p[0], 0.5, 0.03, "and the other half stays in the left front");
        near(p[0] + p[2], 1.0, 0.04, "so front plus centre still reconstructs the input");
        chk(p[4] < 0.01 && p[5] < 0.01,
            "mono content puts nothing in the rears - there is no difference to send");
    }

    // --- the rears carry the difference, and only the difference -------------
    {
        const auto p = peaks(5, 1000.0, 1.0, -1.0);  // pure side content
        chk(p[4] > 0.5, "out-of-phase content reaches the rears");
        near(p[4], p[5], 1e-3, "and the two rears are the same size");
        near(p[2], 0.0, 1e-3, "with nothing in the centre");
    }

    // --- the delay is the length it says it is -------------------------------
    {
        Upmix u; u.configure(kSr);
        const int frames = 4096;
        std::vector<float> in((size_t)frames * 2, 0.0f), out((size_t)frames * 5, 0.0f);
        in[0] = 1.0f; in[1] = -1.0f;                 // one impulse, pure side
        u.process(4, in.data(), out.data(), frames);
        int at = -1;
        for (int f = 0; f < frames && at < 0; ++f)
            if (std::fabs(out[(size_t)f * 5 + 3]) > 0.1f) at = f;
        const int want = int(Upmix::kRearMs * 0.001f * kSr);
        ++g_total;
        if (at != want) {
            ++g_fail;
            std::printf("  FAIL  rear delay landed at %d frames, wanted %d\n", at, want);
        }
    }

    // --- the "only" modes really are only ------------------------------------
    {
        const auto c = peaks(7, 1000.0, 1.0, 0.4);
        chk(c[2] > 0.5, "Centre only puts signal on the centre");
        chk(c[0] < 1e-6 && c[1] < 1e-6 && c[3] < 1e-6 && c[4] < 1e-6 && c[5] < 1e-6,
            "and on nothing else");
        const auto s = peaks(8, 40.0, 1.0, 1.0);
        chk(s[3] > 0.5, "Subwoofer only puts signal on the LFE");
        chk(s[0] < 1e-6 && s[2] < 1e-6 && s[4] < 1e-6, "and on nothing else");
        const auto r = peaks(9, 1000.0, 1.0, -1.0);
        chk(r[4] > 0.5 && r[5] > 0.5, "Rears only puts signal on the rears");
        chk(r[0] < 1e-6 && r[2] < 1e-6 && r[3] < 1e-6, "and on nothing else");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
