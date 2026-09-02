// betterbanana - voice changer presets.
//
// The plain-values mirror of VoiceFx, plus the character presets, shared by the
// GUI and bb-ctl so both offer exactly the same set. Deliberately FX-only: a
// preset never touches the strip's EQ, because silently rewriting a curve
// somebody tuned would be a nasty surprise. The band-limited "telephone" sound
// is an EQ recipe, not an effect - high pass at 300 Hz, low pass at 3.4 kHz.
#pragma once

#include "protocol.h"

#include <cmath>
#include <string>
#include <vector>

namespace bb {

struct FxValues {
    float pitch      = 0.0f;    // semitones
    bool  formant_on = false;
    float formant    = 0.0f;    // net semitones, when formant_on
    float drive      = 0.0f;    // 0 .. 10
    float ring_hz    = 0.0f;
    float ring_mix   = 0.0f;
    int   bits       = 0;       // 0 = off
    int   downsample = 1;       // 1 = off
    float echo_ms    = 0.0f;
    float echo_fb    = 0.0f;
    float echo_mix   = 0.0f;
    float chorus_ms  = 0.0f;
    float chorus_hz  = 0.0f;
    float chorus_mix = 0.0f;
    float gain_db    = 0.0f;
};

inline void fx_apply(VoiceFx& p, const FxValues& v)
{
    auto cl = [](float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); };
    p.pitch.store(cl(v.pitch, -12.0f, 12.0f));
    p.formant_on.store(v.formant_on ? 1 : 0);
    p.formant.store(cl(v.formant, -12.0f, 12.0f));
    p.drive.store(cl(v.drive, 0.0f, 10.0f));
    p.ring_hz.store(cl(v.ring_hz, 0.0f, 2000.0f));
    p.ring_mix.store(cl(v.ring_mix, 0.0f, 1.0f));
    p.bits.store(v.bits <= 0 ? 0 : (v.bits < 2 ? 2 : (v.bits > 15 ? 15 : v.bits)));
    p.downsample.store(v.downsample < 1 ? 1 : (v.downsample > 64 ? 64 : v.downsample));
    p.echo_ms.store(cl(v.echo_ms, 0.0f, 1000.0f));
    p.echo_fb.store(cl(v.echo_fb, 0.0f, 0.95f));
    p.echo_mix.store(cl(v.echo_mix, 0.0f, 1.0f));
    p.chorus_ms.store(cl(v.chorus_ms, 0.0f, 12.0f));
    p.chorus_hz.store(cl(v.chorus_hz, 0.0f, 8.0f));
    p.chorus_mix.store(cl(v.chorus_mix, 0.0f, 1.0f));
    p.gain_db.store(cl(v.gain_db, -24.0f, 24.0f));
}

inline FxValues fx_capture(const VoiceFx& p)
{
    FxValues v;
    v.pitch      = p.pitch.load();
    v.formant_on = p.formant_on.load() != 0;
    v.formant    = p.formant.load();
    v.drive      = p.drive.load();
    v.ring_hz    = p.ring_hz.load();
    v.ring_mix   = p.ring_mix.load();
    v.bits       = p.bits.load();
    v.downsample = p.downsample.load();
    v.echo_ms    = p.echo_ms.load();
    v.echo_fb    = p.echo_fb.load();
    v.echo_mix   = p.echo_mix.load();
    v.chorus_ms  = p.chorus_ms.load();
    v.chorus_hz  = p.chorus_hz.load();
    v.chorus_mix = p.chorus_mix.load();
    v.gain_db    = p.gain_db.load();
    return v;
}

struct FxPreset {
    const char* name;
    FxValues    v;
};

// Somewhere to start from. Every one of these is reachable by hand; they exist
// so the first thing you do is not stare at fourteen knobs.
//
// The first group shifts formants independently, which is what makes a voice
// read as a different person rather than a different size. The second group
// deliberately does NOT - letting the formants ride along with the pitch is
// exactly what makes a chipmunk sound like a chipmunk.
inline const std::vector<FxPreset>& fx_presets()
{
    static const std::vector<FxPreset> kPresets = {
        { "Off",       {} },

        { "Feminine",  { .pitch = 6.0f,  .formant_on = true, .formant = 3.0f,
                         .gain_db = -1.0f } },
        { "Masculine", { .pitch = -5.0f, .formant_on = true, .formant = -2.5f } },
        { "Higher",    { .pitch = 3.0f,  .formant_on = true, .formant = 1.5f } },
        { "Deeper",    { .pitch = -3.0f, .formant_on = true, .formant = -1.5f } },

        { "Chipmunk",  { .pitch = 7.0f,  .gain_db = -1.0f } },
        { "Squeaky",   { .pitch = 12.0f, .gain_db = -1.0f } },
        { "Deep",      { .pitch = -5.0f } },
        { "Demon",     { .pitch = -9.0f, .drive = 3.0f, .echo_ms = 90.0f,
                         .echo_fb = 0.25f, .echo_mix = 0.30f, .gain_db = -2.0f } },
        { "Robot",     { .ring_hz = 60.0f, .ring_mix = 0.85f, .bits = 8,
                         .gain_db = -1.0f } },
        { "Alien",     { .pitch = 4.0f, .ring_hz = 180.0f, .ring_mix = 0.50f,
                         .chorus_ms = 6.0f, .chorus_hz = 0.6f, .chorus_mix = 0.50f,
                         .gain_db = -1.0f } },
        { "Lo-fi",     { .drive = 2.0f, .bits = 6, .downsample = 3, .gain_db = -1.0f } },
        { "Cave",      { .echo_ms = 160.0f, .echo_fb = 0.45f, .echo_mix = 0.40f,
                         .gain_db = -2.0f } },
        { "Detuned",   { .chorus_ms = 8.0f, .chorus_hz = 0.35f, .chorus_mix = 0.60f } },
    };
    return kPresets;
}

inline bool fx_preset_by_name(const std::string& name, FxValues& out)
{
    for (const FxPreset& p : fx_presets())
        if (name == p.name) { out = p.v; return true; }
    return false;
}

// Which preset a block currently matches, or -1 for "something you edited".
// Lets the GUI combo fall back to "(custom)" the moment a knob moves.
inline int fx_preset_index(const FxValues& v)
{
    auto same = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
    const std::vector<FxPreset>& all = fx_presets();
    for (size_t i = 0; i < all.size(); ++i) {
        const FxValues& p = all[i].v;
        if (same(v.pitch, p.pitch) && v.formant_on == p.formant_on
            && (!v.formant_on || same(v.formant, p.formant))
            && same(v.drive, p.drive)
            && same(v.ring_hz, p.ring_hz) && same(v.ring_mix, p.ring_mix)
            && v.bits == p.bits && v.downsample == p.downsample
            && same(v.echo_ms, p.echo_ms) && same(v.echo_fb, p.echo_fb)
            && same(v.echo_mix, p.echo_mix) && same(v.chorus_ms, p.chorus_ms)
            && same(v.chorus_hz, p.chorus_hz) && same(v.chorus_mix, p.chorus_mix)
            && same(v.gain_db, p.gain_db))
            return (int)i;
    }
    return -1;
}

} // namespace bb
