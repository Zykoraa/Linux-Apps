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

// Somewhere to start from. Every one of these is reachable by hand from the
// controls; they exist so the first thing you do is not stare at twelve knobs.
inline const std::vector<FxPreset>& fx_presets()
{
    // pitch drive ring_hz ring_mix bits down echo_ms echo_fb echo_mix
    //   chorus_ms chorus_hz chorus_mix gain
    static const std::vector<FxPreset> kPresets = {
        { "Off",       {   0, 0,   0,    0,   0, 1,   0,    0,    0,    0,    0,    0,   0 } },
        { "Chipmunk",  {  +7, 0,   0,    0,   0, 1,   0,    0,    0,    0,    0,    0,  -1 } },
        { "Squeaky",   { +12, 0,   0,    0,   0, 1,   0,    0,    0,    0,    0,    0,  -1 } },
        { "Deep",      {  -5, 0,   0,    0,   0, 1,   0,    0,    0,    0,    0,    0,   0 } },
        { "Demon",     {  -9, 3,   0,    0,   0, 1,  90, 0.25f, 0.30f,  0,    0,    0,  -2 } },
        { "Robot",     {   0, 0,  60, 0.85f,  8, 1,   0,    0,    0,    0,    0,    0,  -1 } },
        { "Alien",     {  +4, 0, 180, 0.50f,  0, 1,   0,    0,    0,    6, 0.6f, 0.50f, -1 } },
        { "Lo-fi",     {   0, 2,   0,    0,   6, 3,   0,    0,    0,    0,    0,    0,  -1 } },
        { "Cave",      {   0, 0,   0,    0,   0, 1, 160, 0.45f, 0.40f,  0,    0,    0,  -2 } },
        { "Detuned",   {   0, 0,   0,    0,   0, 1,   0,    0,    0,    8, 0.35f,0.60f,  0 } },
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
        if (same(v.pitch, p.pitch) && same(v.drive, p.drive)
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
