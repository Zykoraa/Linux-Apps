// Presets: the text form, the atomic file write, the startup marker, and the
// per-device strip snapshots.
//
// The round-trip test earns its keep twice over: undo is built out of
// serialize/deserialize, so anything the format quietly drops would be a
// control that Ctrl+Z silently fails to restore.
#include "../common/preset.h"
#include "../common/eqprofile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

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

// Gives every serialized field a distinctive value, so a round trip that loses
// one shows up as a difference rather than as a coincidence.
static void scramble(Shared* s)
{
    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        p.gain_db.store(-3.5f * (i + 1));
        p.mute.store(i & 1);
        p.solo.store((i >> 1) & 1);
        p.mono.store(i == 2);
        p.mono_source.store(i == 1);
        p.limit_db.store(6.0f - i);
        p.delay_ms.store(12.0f * i);
        p.gate.store(0.5f * i);
        p.comp.store(1.25f * i);
        p.audibility.store(0.75f * i);
        p.eq_low.store(1.5f * i - 2.0f);
        p.eq_mid.store(-1.0f * i);
        p.eq_high.store(0.5f * i);
        p.pan_x.store(0.1f * i - 0.2f);
        p.pan_y.store(0.05f * i);
        p.duck_key.store(i == 0);
        p.duck_depth_db.store(-4.0f * i);
        for (int b = 0; b < kBuses; ++b) p.bus_on[b].store((i + b) & 1);
        p.eq.on.store(i != 3);
        p.eq.preamp_db.store(-2.5f - i);
        for (int k = 0; k < kEqBands; ++k) {
            p.eq.gain[k].store(0.5f * k - 3.0f * i);
            p.eq.freq[k].store(80.0f * (k + 1) + 7.0f * i);
            p.eq.q[k].store(0.5f + 0.25f * k);
            p.eq.type[k].store((k + i) % kEqTypeCount);
            p.eq.band_on[k].store((k + i) % 3 != 0);
        }
        p.fx.on.store(i != 2);
        p.fx.pitch.store(-3.0f + 2.0f * i);
        p.fx.formant_on.store(i % 2);
        p.fx.formant.store(1.5f - 0.8f * i);
        p.fx.drive.store(0.5f * i);
        p.fx.ring_hz.store(40.0f + 13.0f * i);
        p.fx.ring_mix.store(0.1f * i);
        p.fx.bits.store(i == 0 ? 0 : 4 + i);
        p.fx.downsample.store(1 + i);
        p.fx.echo_ms.store(60.0f * i);
        p.fx.echo_fb.store(0.05f * i);
        p.fx.echo_mix.store(0.15f * i);
        p.fx.chorus_ms.store(1.5f * i);
        p.fx.chorus_hz.store(0.3f + 0.2f * i);
        p.fx.chorus_mix.store(0.12f * i);
        p.fx.reverb_size.store(0.2f + 0.15f * i);
        p.fx.reverb_damp.store(0.9f - 0.1f * i);
        p.fx.reverb_mix.store(0.05f * i);
        p.fx.tune_on.store(i % 2 == 0);
        p.fx.tune_speed_ms.store(20.0f * i);
        p.fx.tune_amount.store(0.15f * i);
        p.fx.tune_key.store(i * 2);
        p.fx.tune_scale.store(i % 3);
        p.fx.gain_db.store(-1.5f * i);
    }
    for (int b = 0; b < kBuses; ++b) {
        BusParams& p = s->bus[b];
        p.gain_db.store(2.0f * b - 5.0f);
        p.mute.store(b == 1);
        p.mono.store(b == 4);
        p.mode.store((b + 1) % kBusModeCount);
        p.delay_ms.store(3.5f * b);
        p.eq.on.store(b != 2);
        p.eq.preamp_db.store(-1.5f * b);
        for (int k = 0; k < kEqBands; ++k) {
            p.eq.gain[k].store(-0.75f * k + b);
            p.eq.freq[k].store(60.0f * (k + 2) + 3.0f * b);
            p.eq.q[k].store(0.7f + 0.3f * k);
            p.eq.type[k].store((k + b + 2) % kEqTypeCount);
            p.eq.band_on[k].store((k + b) % 4 != 0);
        }
    }
    s->duck_enabled.store(1);
    s->duck_threshold_db.store(-27.5f);
    s->duck_attack_ms.store(18.0f);
    s->duck_release_ms.store(410.0f);
    s->rec.source_bus.store(3);
    s->rec.gain_db.store(-4.5f);
    s->rec.loop.store(1);
    for (int b = 0; b < kBuses; ++b) s->rec.bus_on[b].store(b % 2);

    routing_write_begin(s->routing);
    for (int i = 0; i < kHwStrips; ++i) {
        snprintf(s->routing.hw_in[i], kNameLen, "alsa_input.thing_%d.source", i);
        snprintf(s->routing.hw_in_desc[i], kNameLen, "Some Interface %d", i);
    }
    for (int b = 0; b < kPhysBuses; ++b) {
        snprintf(s->routing.bus_out[b], kNameLen, "alsa_output.thing_%d.sink", b);
        snprintf(s->routing.bus_out_desc[b], kNameLen, "Some Speakers %d", b);
    }
    routing_write_end(s->routing);

    s->labels.seq.fetch_add(1);
    snprintf(s->labels.strip[0], kLabelLen, "MIC");
    snprintf(s->labels.bus[1], kLabelLen, "SPEAKERS");
    s->labels.seq.fetch_add(1);

    s->vban.seq.fetch_add(1);
    s->vban.out[0].enabled = 1;
    s->vban.out[0].source_bus = 2;
    s->vban.out[0].port = 6990;
    s->vban.out[0].rate = 48000;
    snprintf(s->vban.out[0].name, sizeof(s->vban.out[0].name), "Studio");
    snprintf(s->vban.out[0].host, sizeof(s->vban.out[0].host), "192.168.1.20");
    s->vban.in[1].enabled = 1;
    s->vban.in[1].port = 6991;
    s->vban.in[1].rate = 48000;
    snprintf(s->vban.in[1].name, sizeof(s->vban.in[1].name), "Booth");
    s->vban.seq.fetch_add(1);
}

int main()
{
    std::printf("test_preset\n");

    char tmpl[] = "/tmp/bbpreset.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { std::printf("  FAIL  cannot make a temp dir\n"); return 1; }
    setenv("XDG_CONFIG_HOME", dir, 1);

    auto shm = std::make_unique<Shared>();
    set_defaults(shm.get());

    // --- the text form is a fixed point ------------------------------------
    scramble(shm.get());
    const std::string first = preset_serialize(shm.get());

    auto blank = std::make_unique<Shared>();
    set_defaults(blank.get());
    chk(preset_deserialize(blank.get(), first), "a serialized preset deserializes");
    const std::string second = preset_serialize(blank.get());
    chk(first == second, "serialize -> deserialize -> serialize is identical");
    if (first != second) {
        // Name the first line that differs; that is the field being dropped.
        size_t a = 0, b = 0;
        while (a < first.size() && b < second.size()) {
            const size_t ae = first.find('\n', a), be = second.find('\n', b);
            if (first.compare(a, ae - a, second, b, be - b) != 0) {
                std::printf("        first difference: %s   vs   %s\n",
                            first.substr(a, ae - a).c_str(), second.substr(b, be - b).c_str());
                break;
            }
            a = ae + 1; b = be + 1;
        }
    }

    // Spot-check the fields the v3 format added, since those are the ones a
    // stale round trip would silently lose.
    near(blank->strip[2].eq.preamp_db.load(), -4.5, 1e-3, "strip EQ preamp round trips");
    near(blank->strip[2].eq.freq[5].load(), shm->strip[2].eq.freq[5].load(), 1e-2,
         "strip EQ band frequency round trips");
    chk(blank->strip[3].eq.on.load() == 0, "a strip EQ left off stays off");
    chk(blank->strip[1].mono_source.load() == 1, "mono-source fold round trips");
    near(blank->strip[4].limit_db.load(), 2.0, 1e-3, "limiter ceiling round trips");
    for (int b = 0; b < kBuses; ++b) {
        chk(blank->bus[b].mode.load() == (b + 1) % kBusModeCount, "bus mode round trips");
        near(blank->bus[b].delay_ms.load(), 3.5 * b, 1e-3, "bus delay round trips");
    }
    near(blank->strip[2].delay_ms.load(), 24.0, 1e-3, "strip delay round trips");
    chk(clamp_delay(9999.0) == 500.0f, "a delay past the ceiling is clamped on load");
    chk(clamp_delay(-5.0) == 0.0f, "and a negative one becomes none");
    near(blank->strip[3].fx.pitch.load(), 3.0, 1e-3, "voice-changer pitch round trips");
    near(blank->strip[3].fx.formant.load(), -0.9, 1e-3, "formant shift round trips");
    chk(blank->strip[3].fx.formant_on.load() == 1, "and its enable flag");
    chk(blank->strip[2].fx.formant_on.load() == 0, "including when it is off");
    near(blank->strip[4].fx.echo_ms.load(), 240.0, 1e-2, "echo time round trips");
    near(blank->strip[3].fx.reverb_size.load(), 0.65, 1e-3, "reverb size round trips");
    near(blank->strip[2].fx.reverb_mix.load(), 0.10, 1e-3, "reverb mix round trips");
    chk(blank->strip[3].fx.tune_key.load() == 6, "the tuning key round trips");
    chk(blank->strip[3].fx.tune_scale.load() == 0, "and the scale");
    near(blank->strip[4].fx.tune_speed_ms.load(), 80.0, 1e-3, "and the retune speed");
    chk(blank->strip[2].fx.tune_on.load() == 1, "and whether correction is on");
    chk(blank->strip[2].fx.on.load() == 0, "a voice changer left off stays off");
    chk(blank->strip[3].fx.downsample.load() == 4, "the crusher's decimation round trips");

    // --- files are written whole or not at all ------------------------------
    const std::string path = std::string(dir) + "/state.bbp";
    chk(save_preset(shm.get(), path.c_str()), "a preset saves");
    chk(access((path + ".tmp").c_str(), F_OK) != 0,
        "the temporary file is gone once the rename lands");
    auto reload = std::make_unique<Shared>();
    set_defaults(reload.get());
    chk(load_preset(reload.get(), path.c_str()), "and loads back");
    chk(preset_serialize(reload.get()) == first, "a file round trip is lossless too");

    chk(!save_preset(shm.get(), "/nonexistent-dir-xyz/state.bbp"),
        "saving into a missing directory fails rather than half-writing");

    // --- an older preset still loads, and resets what it cannot describe -----
    {
        auto v2 = std::make_unique<Shared>();
        set_defaults(v2.get());
        // Give it a strip EQ that the v2 file cannot possibly mention.
        v2->strip[0].eq.on.store(1);
        v2->strip[0].eq.gain[4].store(9.0f);
        v2->strip[0].fx.on.store(1);
        v2->strip[0].fx.pitch.store(-7.0f);
        const std::string old =
            "betterbanana-preset 2\n"
            "strip.0.gain -6.000\n"
            "bus.0.eq 1\n"
            "bus.0.preamp -3.000\n"
            "bus.0.band.0 4.000 120.000 0.700 1 1\n";
        chk(preset_deserialize(v2.get(), old), "a version 2 preset still loads");
        near(v2->strip[0].gain_db.load(), -6.0, 1e-4, "its values are applied");
        near(v2->bus[0].eq.gain[0].load(), 4.0, 1e-4, "its bus band is applied");
        near(v2->bus[0].eq.gain[3].load(), 0.0, 1e-6, "bands it omits are reset");
        near(v2->strip[0].eq.gain[4].load(), 0.0, 1e-6,
             "the strip EQ it cannot describe is reset, not left stale");
        chk(v2->strip[0].eq.on.load() == 0, "and that strip EQ ends up off");
        chk(v2->strip[0].fx.on.load() == 0 && v2->strip[0].fx.pitch.load() == 0.0f,
            "and the voice changer it cannot describe is cleared too");
    }

    // --- migration off the old autosave, before any choice has been made -----
    {
        const std::string old = preset_dir() + "/autosave.bbp";
        mkdir(preset_dir().c_str(), 0755);
        chk(startup_preset_name().empty(), "no startup preset to begin with");
        chk(write_file_atomic(old.c_str(), first), "an old autosave exists");
        std::string as;
        chk(migrate_autosave(&as), "it migrates");
        chk(as == "Previous session", "into a normally-named preset");
        chk(startup_preset_name() == "Previous session", "which becomes the startup choice");
        chk(access(old.c_str(), F_OK) != 0, "and the old file is moved aside");
        chk(!migrate_autosave(nullptr), "migration does not run a second time");
    }

    // --- the startup marker -------------------------------------------------
    chk(set_startup_preset_name("Live"), "a startup preset can be set");
    chk(startup_preset_name() == "Live", "and read back");
    chk(preset_path_for("Live") == presets_path() + "/Live.bbp",
        "a bare name resolves into the preset directory");
    chk(preset_path_for("/tmp/x.bbp") == "/tmp/x.bbp", "a path is left alone");
    chk(set_startup_preset_name(""), "and cleared");
    chk(startup_preset_name().empty(), "clearing leaves no name");

    // Choosing "start with a default mixer" is an answer, not the absence of
    // one: a leftover autosave must not creep back in and undo it.
    {
        const std::string old = preset_dir() + "/autosave.bbp";
        chk(write_file_atomic(old.c_str(), first), "a stale autosave reappears");
        chk(!migrate_autosave(nullptr), "it does not migrate over a cleared choice");
        chk(startup_preset_name().empty(), "which stays cleared");
        unlink(old.c_str());
    }

    // --- a preset older than the bus mode must clear one, not keep it ---------
    // Otherwise loading a stereo preset over a 5.1 one leaves the bus quietly
    // republishing a six-channel node that nothing asked for.
    {
        auto old8 = std::make_unique<Shared>();
        set_defaults(old8.get());
        old8->bus[0].mode.store(kBusUpMix51);
        old8->bus[0].delay_ms.store(120.0f);
        chk(preset_deserialize(old8.get(),
                "betterbanana-preset 7\nbus.0.gain 0.000\n"),
            "a preset from before the bus mode loads");
        chk(old8->bus[0].mode.load() == kBusNormal,
            "and puts the bus back to stereo rather than leaving it in surround");
        chk(old8->bus[0].delay_ms.load() == 0.0f,
            "and clears an alignment it does not describe");
    }

    // --- standing the watchdog down -------------------------------------------
    // bb-health reads this file with float(), so the format is part of the
    // contract and not just an implementation detail: a bare unix deadline,
    // nothing else on the line.
    {
        const std::string p = health_inhibit_path();
        chk(p == preset_dir() + "/health-inhibit", "the marker has a fixed name");

        chk(hold_health_watchdog(45), "the watchdog can be held off");
        FILE* f = fopen(p.c_str(), "r");
        chk(f != nullptr, "which writes the marker");
        long long deadline = -1;
        char extra[64] = {};
        const int got = f ? fscanf(f, "%lld%63s", &deadline, extra) : 0;
        if (f) fclose(f);
        chk(got >= 1, "holding a number bb-health can parse");
        chk(extra[0] == '\0', "and nothing else on the line");
        chk(deadline > (long long)time(nullptr), "with the deadline in the future");
        chk(deadline <= (long long)time(nullptr) + 45, "and no further out than asked");

        chk(hold_health_watchdog(0), "and released again");
        chk(fopen(p.c_str(), "r") == nullptr, "which removes the marker");
        chk(hold_health_watchdog(0), "releasing twice is not an error");
    }

    // --- per-device strip snapshots -------------------------------------------
    {
        const std::string dev = "alsa_input.usb-Some_Mic-00.mono-fallback";
        chk(!has_strip_for_device(dev), "an unknown device has nothing remembered");
        chk(save_strip_for_device(shm.get(), 0, dev), "a strip can be remembered for a device");
        chk(has_strip_for_device(dev), "and is found afterwards");

        auto other = std::make_unique<Shared>();
        set_defaults(other.get());
        // Bus assignment belongs to the mix, so it must NOT travel with the mic.
        for (int b = 0; b < kBuses; ++b) other->strip[1].bus_on[b].store(b == 4);
        chk(load_strip_for_device(other.get(), 1, dev), "and restores onto another strip");
        near(other->strip[1].gain_db.load(), shm->strip[0].gain_db.load(), 1e-3,
             "level travels with the device");
        near(other->strip[1].gate.load(), shm->strip[0].gate.load(), 1e-3,
             "the gate travels with the device");
        near(other->strip[1].eq.freq[7].load(), shm->strip[0].eq.freq[7].load(), 1e-2,
             "the parametric EQ travels with the device");
        near(other->strip[1].fx.pitch.load(), shm->strip[0].fx.pitch.load(), 1e-3,
             "and so does the voice changer");
        chk(other->strip[1].bus_on[4].load() == 1 && other->strip[1].bus_on[0].load() == 0,
            "but bus assignment does not");

        chk(forget_strip_for_device(dev), "it can be forgotten");
        chk(!has_strip_for_device(dev), "and is gone afterwards");
    }

    // --- filenames survive the characters node names actually contain ---------
    chk(path_escape("alsa_output.pci-0000_16_00.6.pro-output-0")
            == "alsa_output.pci-0000_16_00.6.pro-output-0",
        "an ordinary node name escapes to itself");
    chk(path_escape("a/b c") == "a%2Fb%20c", "slashes and spaces are escaped");
    chk(path_escape("HiFi__Mic1__source") == "HiFi__Mic1__source", "underscores pass through");

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
