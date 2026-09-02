// Tests for the EQ profile layer: the Equalizer APO / Peace / AutoEq text
// format, the preamp calculator, and moving a profile on and off a bus.
#include "../common/eqprofile.h"

#include <cstdio>
#include <cstring>
using namespace bb;

static int fails = 0;
static void chk(bool ok, const char* what, double got, double want, double tol)
{
    if (!ok) { std::printf("  FAIL  %-46s got %10.4f want %10.4f (tol %g)\n", what, got, want, tol); ++fails; }
    else       std::printf("  ok    %-46s %10.4f\n", what, got);
}
static void near(double got, double want, double tol, const char* what)
{ chk(std::fabs(got - want) <= tol, what, got, want, tol); }
static void yes(bool ok, const char* what)
{
    if (!ok) { std::printf("  FAIL  %s\n", what); ++fails; }
    else       std::printf("  ok    %s\n", what);
}

// oratory1990's published correction for the HD 650, byte for byte.
static const char* kHd650 =
    "Preamp: -6.1 dB\n"
    "Filter 1: ON LSC Fc 105 Hz Gain 6.4 dB Q 0.70\n"
    "Filter 2: ON PK Fc 8800 Hz Gain 5.1 dB Q 1.42\n"
    "Filter 3: ON PK Fc 118 Hz Gain -3.1 dB Q 0.50\n"
    "Filter 4: ON PK Fc 37 Hz Gain 0.7 dB Q 3.96\n"
    "Filter 5: ON PK Fc 3169 Hz Gain -1.7 dB Q 3.89\n"
    "Filter 6: ON HSC Fc 10000 Hz Gain -2.1 dB Q 0.70\n"
    "Filter 7: ON PK Fc 1227 Hz Gain -1.2 dB Q 2.53\n"
    "Filter 8: ON PK Fc 2055 Hz Gain 1.2 dB Q 3.23\n"
    "Filter 9: ON PK Fc 587 Hz Gain 0.4 dB Q 1.19\n"
    "Filter 10: ON PK Fc 5332 Hz Gain -1.1 dB Q 5.75\n";

int main()
{
    std::printf("\n[AutoEq parametric import]\n");
    EqProfile hd = eq_parse_apo(kHd650);
    chk(hd.bands.size() == 10, "ten filters parsed", double(hd.bands.size()), 10, 0);
    near(hd.preamp, -6.1, 1e-4, "preamp read from the header");
    chk(hd.bands[0].type == kEqLowShelf, "LSC -> low shelf", hd.bands[0].type, kEqLowShelf, 0);
    chk(hd.bands[5].type == kEqHighShelf, "HSC -> high shelf", hd.bands[5].type, kEqHighShelf, 0);
    near(hd.bands[1].freq, 8800.0, 1e-3, "band 2 centre frequency");
    near(hd.bands[1].gain, 5.1,    1e-4, "band 2 gain");
    near(hd.bands[1].q,    1.42,   1e-4, "band 2 Q");
    // AutoEq computes its own preamp; ours should land on the same value.
    near(eq_suggest_preamp(hd), -6.1, 0.35, "our preamp matches AutoEq's");

    std::printf("\n[round trip through the file format]\n");
    {
        EqProfile back = eq_parse_apo(eq_format_apo(hd));
        chk(back.bands.size() == hd.bands.size(), "band count survives",
            double(back.bands.size()), double(hd.bands.size()), 0);
        near(back.preamp, hd.preamp, 1e-4, "preamp survives");
        double worst = 0.0;
        for (size_t i = 0; i < hd.bands.size(); ++i) {
            worst = std::max(worst, (double)std::fabs(back.bands[i].freq - hd.bands[i].freq));
            worst = std::max(worst, (double)std::fabs(back.bands[i].gain - hd.bands[i].gain));
            worst = std::max(worst, (double)std::fabs(back.bands[i].q    - hd.bands[i].q));
            if (back.bands[i].type != hd.bands[i].type) worst = 1e9;
        }
        near(worst, 0.0, 0.01, "every band field survives");
    }

    std::printf("\n[the spellings other tools use]\n");
    {
        EqProfile p = eq_parse_apo(
            "Preamp: -3 dB\n"
            "Filter 1: ON LS 6dB Fc 100 Hz Gain 4 dB\n"      // first-order shelf
            "Filter 2: OFF PK Fc 1000 Hz Gain 2 dB Q 1\n"    // bypassed band
            "Filter 3: ON LPQ Fc 12000 Hz Q 0.7\n"           // no Gain field
            "Filter 4: ON PK Fc 500 Hz Gain -2 dB BW Oct 1.0\n"  // width in octaves
            "Filter 5: ON AP Fc 200 Hz Q 1\n"                // all-pass: unsupported
            "Filter 6: ON None\n");                          // empty slot
        chk(p.bands.size() == 4, "all-pass and empty slots dropped",
            double(p.bands.size()), 4, 0);
        yes(p.bands[0].type == kEqLowShelf, "\"LS 6dB\" reads as a low shelf");
        yes(!p.bands[1].on, "OFF is carried through as a bypassed band");
        yes(p.bands[2].type == kEqLowPass, "a low-pass with no Gain field parses");
        near(p.bands[3].q, 1.4142, 0.001, "1 octave bandwidth -> Q 1.414");
    }

    std::printf("\n[gain-free filter shapes]\n");
    {
        const float sr = 48000.0f;
        Biquad b;
        design_band(b, kEqHighPass, sr, 100.0f, 0.707f, 0.0f);
        near(b.magnitude_db(sr, 5000.0f),  0.0, 0.1, "high-pass passes above the corner");
        chk(b.magnitude_db(sr, 20.0f) < -20.0, "high-pass stops below it",
            b.magnitude_db(sr, 20.0f), -20.0, 0);
        design_band(b, kEqNotch, sr, 1000.0f, 8.0f, 0.0f);
        chk(b.magnitude_db(sr, 1000.0f) < -40.0, "notch kills its centre",
            b.magnitude_db(sr, 1000.0f), -40.0, 0);
        near(b.magnitude_db(sr, 100.0f), 0.0, 0.1, "notch leaves the rest alone");
        design_band(b, kEqBandPass, sr, 1000.0f, 2.0f, 0.0f);
        near(b.magnitude_db(sr, 1000.0f), 0.0, 0.1, "band-pass is unity at centre");
        chk(b.magnitude_db(sr, 60.0f) < -20.0, "band-pass rejects far below",
            b.magnitude_db(sr, 60.0f), -20.0, 0);
    }

    std::printf("\n[applying to a bus]\n");
    {
        Shared* s = new Shared();
        set_defaults(s);
        eq_apply(s->bus[0].eq, hd);
        near(s->bus[0].eq.preamp_db.load(), -6.1, 1e-4, "preamp reaches the bus");
        near(s->bus[0].eq.freq[1].load(), 8800.0, 1e-3, "band 2 frequency reaches the bus");
        chk(s->bus[0].eq.type[0].load() == kEqLowShelf, "band 1 type reaches the bus",
            s->bus[0].eq.type[0].load(), kEqLowShelf, 0);
        // Ten filters into twelve slots: the spare two must come back flat, not
        // hold whatever the previous profile left there.
        near(s->bus[0].eq.gain[10].load(), 0.0, 1e-6, "unused band 11 is flat");
        near(s->bus[0].eq.gain[11].load(), 0.0, 1e-6, "unused band 12 is flat");

        EqProfile round = eq_capture(s->bus[0].eq);
        double worst = 0.0;
        for (size_t i = 0; i < hd.bands.size(); ++i)
            worst = std::max(worst, (double)std::fabs(round.bands[i].gain - hd.bands[i].gain));
        near(worst, 0.0, 1e-4, "capture returns what was applied");

        // A profile with more bands than the hardware keeps the strongest.
        EqProfile big;
        big.preamp = -2.0f;
        for (int i = 0; i < 20; ++i) {
            EqBand b;
            b.freq = 30.0f * std::pow(1.35f, (float)i);
            b.gain = (i % 2 ? 1.0f : -1.0f) * (0.2f + 0.4f * i);   // rises with i
            b.q = 1.0f;
            big.bands.push_back(b);
        }
        const std::vector<EqBand> fitted = eq_fit_bands(big.bands);
        chk(fitted.size() == kEqBands, "an oversized profile is trimmed to fit",
            double(fitted.size()), kEqBands, 0);
        double smallest = 1e9;
        for (const EqBand& b : fitted) smallest = std::min(smallest, (double)std::fabs(b.gain));
        chk(smallest > 3.0, "the trimmed bands are the loudest ones", smallest, 3.0, 0);
        bool ordered = true;
        for (size_t i = 1; i < fitted.size(); ++i)
            if (fitted[i].freq < fitted[i - 1].freq) ordered = false;
        yes(ordered, "trimming keeps the bands in frequency order");

        // A pass filter shapes the whole curve, so it survives trimming even
        // though its gain field is zero.
        EqProfile withHp = big;
        EqBand hp; hp.type = kEqHighPass; hp.freq = 40.0f; hp.gain = 0.0f; hp.q = 0.7f;
        withHp.bands.insert(withHp.bands.begin(), hp);
        const std::vector<EqBand> keptHp = eq_fit_bands(withHp.bands);
        bool foundHp = false;
        for (const EqBand& b : keptHp) if (b.type == kEqHighPass) foundHp = true;
        yes(foundHp, "a high-pass is never trimmed away for having no gain");

        delete s;
    }

    std::printf("\n[built-in presets]\n");
    {
        int bad = 0;
        for (const EqFactoryPreset& fp : eq_factory_presets()) {
            EqProfile p;
            if (!eq_factory_profile(fp.name, p)) { ++bad; continue; }
            // Every built-in must be unity-safe: its own preamp has to hold the
            // whole curve at or below 0 dB.
            float peak = -1e9f;
            for (int i = 0; i <= 240; ++i)
                peak = std::max(peak, eq_response_db(p, 20.0f * std::pow(1000.0f, i / 240.0f)));
            if (peak > 0.05f) {
                std::printf("  FAIL  built-in \"%s\" peaks at %+.2f dB\n", fp.name, peak);
                ++bad;
            }
        }
        chk(bad == 0, "every built-in preset is clipping-safe", bad, 0, 0);
        EqProfile miss;
        yes(!eq_factory_profile("Nonexistent", miss), "an unknown preset name is refused");
    }

    std::printf("\n[preset file compatibility]\n");
    {
        // A version 1 preset describes six peaking bands and no preamp. Loading
        // one must clear the other six slots rather than leave a previous
        // profile's tail behind.
        Shared* s = new Shared();
        set_defaults(s);
        eq_apply(s->bus[0].eq, hd);          // leaves band 8 at 2055 Hz / +1.2 dB

        const char* path = "/tmp/bb-test-v1.bbp";
        FILE* f = fopen(path, "w");
        yes(f != nullptr, "test preset file created");
        if (f) {
            fprintf(f, "betterbanana-preset 1\n");
            for (int k = 0; k < 6; ++k)
                fprintf(f, "bus.0.band.%d %.3f %.3f %.3f\n", k, 0.0, 100.0 * (k + 1), 1.0);
            fclose(f);
            yes(load_preset(s, path), "a version 1 preset still loads");
            near(s->bus[0].eq.freq[0].load(), 100.0, 1e-3, "its six bands are applied");
            near(s->bus[0].eq.gain[7].load(), 0.0, 1e-6, "band 8 is reset, not left stale");
            near(s->bus[0].eq.preamp_db.load(), 0.0, 1e-6, "preamp resets when absent");
            chk(s->bus[0].eq.type[7].load() == kEqPeak, "reset bands return to peaking",
                s->bus[0].eq.type[7].load(), kEqPeak, 0);
            remove(path);
        }

        // A version 2 round trip must preserve everything, types included.
        set_defaults(s);
        eq_apply(s->bus[1].eq, hd);
        s->bus[1].eq.band_on[3].store(0);
        const char* p2 = "/tmp/bb-test-v2.bbp";
        yes(save_preset(s, p2), "current state saves");
        set_defaults(s);
        yes(load_preset(s, p2), "and loads back");
        near(s->bus[1].eq.preamp_db.load(), -6.1, 1e-3, "preamp round trips");
        chk(s->bus[1].eq.type[0].load() == kEqLowShelf, "band type round trips",
            s->bus[1].eq.type[0].load(), kEqLowShelf, 0);
        chk(s->bus[1].eq.band_on[3].load() == 0, "a bypassed band round trips",
            s->bus[1].eq.band_on[3].load(), 0, 0);
        remove(p2);
        delete s;
    }

    std::printf("\n%s (%d failure%s)\n\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
