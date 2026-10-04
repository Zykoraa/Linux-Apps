// The mix matrix: post-fader routing as it always was, and the pre-fader send.
//
// The point of the send is one sentence - the fader sets what you hear, the
// send sets what the stream hears - so most of these pin a consequence of it:
// a fader at the bottom still reaches a pre-fader bus at full level, mute still
// silences it, solo does not cut it.
#include "../engine/matrix.h"

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
        std::printf("  FAIL  %s: got %.6f want %.6f\n", what, got, want);
    }
}

constexpr float kSr = 48000.0f;
constexpr uint32_t kN = 256;

// One strip, one block, five buses. post/pre are constant-valued so every
// sample of an accumulator says exactly what reached it.
struct Rig {
    std::vector<float> acc[kBuses], post, pre, dg;
    float* accp[kBuses];
    BusRoute route[kBuses];
    float target[kBuses];
    StripSends sends;

    Rig(float postv, float prev)
    {
        for (int b = 0; b < kBuses; ++b) { acc[b].assign(kN * kChan, 0.0f); accp[b] = acc[b].data(); }
        post.assign(kN * kChan, postv);
        pre.assign(kN * kChan, prev);
        dg.assign(kN, 1.0f);
        for (int b = 0; b < kBuses; ++b) { target[b] = 1.0f; route[b] = BusRoute{}; }
        sends.configure(kSr);
    }
    void clear() { for (int b = 0; b < kBuses; ++b) std::fill(acc[b].begin(), acc[b].end(), 0.0f); }
    void run(bool with_pre = true)
    {
        matrix_add_strip(accp, post.data(), with_pre ? pre.data() : nullptr, dg.data(), kN,
                         route, target, sends);
    }
    float last(int b) const { return acc[b][(kN - 1) * kChan]; }
    float first(int b) const { return acc[b][0]; }
};

int main()
{
    // --- post-fader routing is exactly the old sum -------------------------
    {
        Rig r(0.25f, 0.9f);
        r.route[0].on = true;
        r.route[2].on = true;
        r.dg.assign(kN, 0.5f);
        r.run();
        near(r.first(0), 0.125, 1e-7, "a post-fader bus gets the strip after its fader, ducked");
        near(r.last(2), 0.125, 1e-7, "on every bus it is routed to");
        near(r.last(1), 0.0, 0.0, "and nothing reaches a bus it is not routed to");
    }

    // --- the fader does not reach a pre-fader bus --------------------------
    // The whole feature: a fader pulled to the bottom (post = 0) still sends
    // the strip to a pre-fader bus at its send level.
    {
        Rig r(0.0f, 0.5f);
        r.route[2] = BusRoute{ true, true, false };
        r.route[0] = BusRoute{ true, false, false };
        r.target[2] = send_target(0.0f, false);
        r.run();
        near(r.last(2), 0.5, 1e-6, "a pre-fader bus ignores the fader");
        near(r.last(0), 0.0, 0.0, "while a post-fader bus follows it");

        r.clear();
        r.target[2] = send_target(-6.0f, false);
        for (int k = 0; k < 40; ++k) { r.clear(); r.run(); }   // let the 15 ms ramp settle
        near(r.last(2), 0.5 * std::pow(10.0, -6.0 / 20.0), 1e-4, "at its own send level");
    }

    // --- a send move ramps rather than steps -------------------------------
    {
        Rig r(0.0f, 1.0f);
        r.route[2] = BusRoute{ true, true, false };
        r.target[2] = 1.0f;
        r.run();
        r.clear();
        r.target[2] = 0.0f;
        r.run();
        chk(r.first(2) > 0.99f, "a send turned down starts from where it was");
        chk(r.last(2) < r.first(2) && r.last(2) > 0.0f, "and ramps down rather than stepping");
        float worst = 0.0f;
        for (uint32_t f = 1; f < kN; ++f)
            worst = std::max(worst, std::fabs(r.acc[2][f * kChan] - r.acc[2][(f - 1) * kChan]));
        chk(worst < 0.01f, "with no sample-to-sample jump a click would be made of");
    }

    // --- mute silences the send too ----------------------------------------
    {
        chk(send_target(0.0f, true) == 0.0f, "a muted strip sends nothing");
        chk(send_target(kSendOffDb, false) == 0.0f, "a send at the bottom is off, not -60 dB");
        chk(send_target(-200.0f, false) == 0.0f, "and so is one below it");
        near(send_target(40.0f, false), std::pow(10.0, kSendMaxDb / 20.0), 1e-4,
             "a send above the ceiling is held to it");

        Rig r(0.0f, 1.0f);
        r.route[2] = BusRoute{ true, true, false };
        r.target[2] = send_target(0.0f, true);
        for (int k = 0; k < 20; ++k) { r.clear(); r.run(); }   // ~107 ms, 7 time constants
        chk(std::fabs(r.last(2)) < 1e-3f, "mute ramps the send to silence");
    }

    // --- solo does not cut a pre-fader bus ---------------------------------
    // mix_chunk never marks a pre-fader bus solo-blocked; this pins what a
    // blocked route does so that rule is the only thing standing between solo
    // and the stream.
    {
        Rig r(0.3f, 0.3f);
        r.route[0] = BusRoute{ true, false, true };
        r.route[2] = BusRoute{ true, true, false };
        r.run();
        near(r.last(0), 0.0, 0.0, "a solo-blocked post-fader bus gets nothing");
        near(r.last(2), 0.3, 1e-6, "an unblocked pre-fader bus still gets the strip");
    }

    // --- ducking reaches both paths ----------------------------------------
    {
        Rig r(0.4f, 0.8f);
        r.route[0] = BusRoute{ true, false, false };
        r.route[2] = BusRoute{ true, true, false };
        r.dg.assign(kN, 0.25f);
        r.run();
        near(r.last(0), 0.1, 1e-6, "the ducker pulls down a post-fader bus");
        near(r.last(2), 0.2, 1e-6, "and a pre-fader one");
    }

    // --- a send routed on starts at its own level --------------------------
    {
        Rig r(0.0f, 1.0f);
        r.route[2] = BusRoute{ false, true, false };
        r.target[2] = send_target(-12.0f, false);
        r.run();                                   // not routed: parks at target
        r.route[2].on = true;
        r.clear();
        r.run();
        near(r.first(2), std::pow(10.0, -12.0 / 20.0), 1e-4,
             "routing a strip on starts the send at its level, not at a stale one");
    }

    // --- without a tap a pre-fader route falls back to post ----------------
    {
        Rig r(0.6f, 0.9f);
        r.route[2] = BusRoute{ true, true, false };
        r.run(false);
        near(r.last(2), 0.6, 1e-6, "no pre-fader tap means the post-fader feed, not silence");
    }

    // --- stereo stays stereo -----------------------------------------------
    {
        Rig r(0.0f, 0.0f);
        for (uint32_t f = 0; f < kN; ++f) { r.pre[f * kChan] = 0.7f; r.pre[f * kChan + 1] = -0.2f; }
        r.route[2] = BusRoute{ true, true, false };
        r.run();
        near(r.acc[2][(kN - 1) * kChan], 0.7, 1e-6, "the left channel goes to the left");
        near(r.acc[2][(kN - 1) * kChan + 1], -0.2, 1e-6, "and the right to the right");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
