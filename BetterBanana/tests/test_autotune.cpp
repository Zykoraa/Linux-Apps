// Note snapping: the part that decides whether pitch correction sounds musical
// or merely in tune.
#include "../engine/autotune.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void near(double got, double want, double tol, const char* what)
{
    ++g_total;
    if (std::fabs(got - want) > tol) {
        ++g_fail;
        std::printf("  FAIL  %s: got %+.3f want %+.3f\n", what, got, want);
    }
}

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static float hz_of(double midi) { return float(440.0 * std::pow(2.0, (midi - 69.0) / 12.0)); }

int main()
{
    std::printf("test_autotune\n");

    // --- chromatic ----------------------------------------------------------
    near(tune_correction(440.0f, 0, kTuneChromatic), 0.0, 1e-4, "A4 is already in tune");
    near(tune_correction(hz_of(69.4), 0, kTuneChromatic), -0.4, 1e-3, "40 cents sharp pulls down");
    near(tune_correction(hz_of(68.7), 0, kTuneChromatic), 0.3, 1e-3, "30 cents flat pulls up");
    // Not the exact halfway point: 69.5 does not survive a log2 round trip, and
    // asserting a rounding tie tests the arithmetic rather than the music.
    near(std::fabs(tune_correction(hz_of(69.45), 0, kTuneChromatic)), 0.45, 1e-3,
         "just under halfway still moves less than a quarter tone");
    near(tune_correction(hz_of(60.0), 0, kTuneChromatic), 0.0, 1e-4, "middle C needs nothing");

    // Correction is never more than half a semitone in chromatic.
    {
        bool ok = true;
        for (double m = 40.0; m < 84.0; m += 0.017)
            if (std::fabs(tune_correction(hz_of(m), 0, kTuneChromatic)) > 0.5001) ok = false;
        chk(ok, "chromatic never moves a note more than 50 cents");
    }

    // --- scales -------------------------------------------------------------
    // C major has no C#. A voice on C# is pulled to C or D, whichever is nearer.
    // C# is not in C major, so it goes to whichever neighbour is nearer - which
    // for anything above 61.0 is D, not C.
    near(tune_correction(hz_of(61.2), 0, kTuneMajor), 0.8, 1e-3, "a high C# in C major rises to D");
    near(tune_correction(hz_of(60.6), 0, kTuneMajor), -0.6, 1e-3, "a low one falls back to C");
    near(tune_correction(hz_of(64.0), 0, kTuneMajor), 0.0, 1e-4, "E is in C major already");
    // D# sits exactly between D and E in C major. Either is defensible; what
    // matters is that it picks one and always the same one.
    near(std::fabs(tune_correction(hz_of(63.0), 0, kTuneMajor)), 1.0, 1e-3,
         "D# in C major moves a whole tone to a neighbour");
    near(tune_correction(hz_of(63.0), 0, kTuneMajor),
         tune_correction(hz_of(63.0), 0, kTuneMajor), 0.0, "and does so deterministically");

    // A minor is the same notes as C major, so the same input snaps the same way.
    near(tune_correction(hz_of(63.0), 9, kTuneMinor),
         tune_correction(hz_of(63.0), 0, kTuneMajor), 1e-4,
         "A minor and C major agree, being the same notes");

    // In A major, C# IS in the scale and must be left alone.
    near(tune_correction(hz_of(61.0), 9, kTuneMajor), 0.0, 1e-4, "C# is in A major");

    {
        // A scale never moves a note more than a whole tone.
        bool ok = true;
        for (int key = 0; key < 12; ++key)
            for (double m = 40.0; m < 84.0; m += 0.037)
                if (std::fabs(tune_correction(hz_of(m), key, kTuneMinor)) > 1.0001) ok = false;
        chk(ok, "a scale never moves a note more than a whole tone");
    }

    // --- guards -------------------------------------------------------------
    near(tune_correction(0.0f, 0, kTuneChromatic), 0.0, 1e-9, "no pitch, no correction");
    near(tune_correction(10.0f, 0, kTuneChromatic), 0.0, 1e-9, "sub-audio is left alone");
    near(tune_correction(5000.0f, 0, kTuneChromatic), 0.0, 1e-9, "and so is anything above a voice");

    // --- naming -------------------------------------------------------------
    chk(std::string(tune_note_name(tune_note_of(440.0f, 0.0f))) == "A", "440 Hz is called A");
    chk(std::string(tune_note_name(tune_note_of(hz_of(60.0), 0.0f))) == "C", "middle C is called C");
    chk(std::string(tune_note_name(-1)) == "-", "no note has no name");

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
