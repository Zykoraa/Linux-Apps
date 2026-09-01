// betterbanana - EQ profiles: the band list of one bus, on its own.
//
// A profile is what a preset file, an Equalizer APO / Peace export and an
// AutoEq download all reduce to, so the GUI and bb-ctl share one parser and
// one applier. The on-disk format IS the Equalizer APO ParametricEQ format,
// unchanged - the same text AutoEq publishes and Peace consumes - so a saved
// BetterBanana profile can be pasted straight into Peace and vice versa.
#pragma once

#include "preset.h"          // preset_dir()
#include "../engine/dsp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bb {

struct EqBand {
    int   type = kEqPeak;
    bool  on   = true;
    float freq = 1000.0f;
    float gain = 0.0f;
    float q    = 1.0f;
};

struct EqProfile {
    std::string name;
    float preamp = 0.0f;
    std::vector<EqBand> bands;
};

// Equalizer APO's own spelling of each shape. LSC/HSC are what AutoEq and
// oratory1990 emit; LS/HS are Peace's older spelling of the same thing.
inline const char* eq_type_tag(int t)
{
    switch (t) {
        case kEqLowShelf:  return "LSC";
        case kEqHighShelf: return "HSC";
        case kEqHighPass:  return "HPQ";
        case kEqLowPass:   return "LPQ";
        case kEqNotch:     return "NO";
        case kEqBandPass:  return "BP";
        default:           return "PK";
    }
}

// Short human label for the GUI's per-band type selector.
inline const char* eq_type_name(int t)
{
    switch (t) {
        case kEqLowShelf:  return "Low shelf";
        case kEqHighShelf: return "High shelf";
        case kEqHighPass:  return "High pass";
        case kEqLowPass:   return "Low pass";
        case kEqNotch:     return "Notch";
        case kEqBandPass:  return "Band pass";
        default:           return "Peak";
    }
}

// True for the shapes whose gain field means anything. The others are pure
// filters, so the GUI greys their gain control out and the engine ignores it.
inline bool eq_type_uses_gain(int t)
{
    return t == kEqPeak || t == kEqLowShelf || t == kEqHighShelf;
}

inline int eq_type_from_tag(const std::string& tag)
{
    std::string t;
    for (char c : tag) t += (char)std::toupper((unsigned char)c);
    if (t == "PK" || t == "PEQ" || t == "MODAL") return kEqPeak;
    if (t == "LS" || t == "LSC" || t == "LSQ")   return kEqLowShelf;
    if (t == "HS" || t == "HSC" || t == "HSQ")   return kEqHighShelf;
    if (t == "HP" || t == "HPQ")                 return kEqHighPass;
    if (t == "LP" || t == "LPQ")                 return kEqLowPass;
    if (t == "NO" || t == "NOTCH")               return kEqNotch;
    if (t == "BP" || t == "BPQ")                 return kEqBandPass;
    return -1;                                    // AP, None, or something new
}

// ---------------------------------------------------------------------------
// Equalizer APO / Peace / AutoEq text.
//
//   Preamp: -6.1 dB
//   Filter 1: ON LSC Fc 105 Hz Gain 6.4 dB Q 0.70
//   Filter 2: ON PK Fc 8800 Hz Gain 5.1 dB Q 1.42
//
// Parsed by keyword rather than by position, because the fields genuinely
// vary: a low-pass carries no Gain, and the first-order shelves spell their
// type as two tokens ("LS 6dB").
// ---------------------------------------------------------------------------
inline EqProfile eq_parse_apo(const std::string& text)
{
    EqProfile prof;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        // Split on whitespace and on the colon that ends "Preamp"/"Filter N".
        std::vector<std::string> tok;
        std::string cur;
        for (char c : line) {
            if (std::isspace((unsigned char)c) || c == ':') {
                if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
            } else cur += c;
        }
        if (!cur.empty()) tok.push_back(cur);
        if (tok.empty()) continue;

        std::string head;
        for (char c : tok[0]) head += (char)std::toupper((unsigned char)c);

        if (head == "PREAMP") {
            if (tok.size() >= 2) prof.preamp = (float)atof(tok[1].c_str());
            continue;
        }
        if (head != "FILTER") continue;

        EqBand b;
        size_t i = 1;
        if (i < tok.size() && std::isdigit((unsigned char)tok[i][0])) ++i;   // filter number
        if (i < tok.size()) {
            std::string st;
            for (char c : tok[i]) st += (char)std::toupper((unsigned char)c);
            if (st == "ON")       { b.on = true;  ++i; }
            else if (st == "OFF") { b.on = false; ++i; }
        }
        if (i >= tok.size()) continue;
        const int type = eq_type_from_tag(tok[i]);
        if (type < 0) continue;                      // all-pass and friends: drop
        b.type = type;
        ++i;
        // Swallow the slope token of "LS 6dB" / "HS 12dB", and take its slope as
        // the Q, since those spellings never carry an explicit Q.
        if (i < tok.size() && tok[i].size() >= 3
            && std::isdigit((unsigned char)tok[i][0])
            && tok[i].find("dB") != std::string::npos
            && tok[i].find("dB") == tok[i].size() - 2) {
            b.q = atoi(tok[i].c_str()) <= 6 ? 0.5f : 0.707f;
            ++i;
        }

        bool have_q = false;
        for (; i < tok.size(); ++i) {
            std::string k;
            for (char c : tok[i]) k += (char)std::toupper((unsigned char)c);
            if (k == "FC" && i + 1 < tok.size())        b.freq = (float)atof(tok[++i].c_str());
            else if (k == "GAIN" && i + 1 < tok.size()) b.gain = (float)atof(tok[++i].c_str());
            else if (k == "Q" && i + 1 < tok.size())  { b.q = (float)atof(tok[++i].c_str()); have_q = true; }
            else if (k == "BW" && i + 2 < tok.size() && tok[i + 1] == "Oct") {
                // Bandwidth in octaves, the other way Equalizer APO spells width.
                const double bw = atof(tok[i + 2].c_str());
                if (bw > 0.01) {
                    const double t = std::pow(2.0, bw);
                    b.q = (float)(std::sqrt(t) / (t - 1.0));
                    have_q = true;
                }
                i += 2;
            }
        }
        if (!have_q && b.q <= 0.0f) b.q = 0.707f;
        if (b.freq <= 0.0f) continue;
        prof.bands.push_back(b);
    }
    return prof;
}

inline std::string eq_format_apo(const EqProfile& prof)
{
    char buf[256];
    std::string out;
    if (!prof.name.empty()) {
        std::snprintf(buf, sizeof(buf), "# %s\n", prof.name.c_str());
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "Preamp: %.1f dB\n", prof.preamp);
    out += buf;
    int n = 0;
    for (const EqBand& b : prof.bands) {
        ++n;
        if (eq_type_uses_gain(b.type))
            std::snprintf(buf, sizeof(buf), "Filter %d: %s %s Fc %g Hz Gain %.1f dB Q %.2f\n",
                          n, b.on ? "ON" : "OFF", eq_type_tag(b.type), b.freq, b.gain, b.q);
        else
            std::snprintf(buf, sizeof(buf), "Filter %d: %s %s Fc %g Hz Q %.2f\n",
                          n, b.on ? "ON" : "OFF", eq_type_tag(b.type), b.freq, b.q);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Curve evaluation. Shared by the GUI's display, the preamp calculator and the
// tests, and built from the same Biquad the engine runs.
// ---------------------------------------------------------------------------
inline float eq_response_db(const EqProfile& prof, float freq, float sr = 48000.0f)
{
    float db = prof.preamp;
    Biquad bq;
    for (const EqBand& b : prof.bands) {
        if (!b.on) continue;
        design_band(bq, b.type, sr, b.freq, b.q, b.gain);
        db += bq.magnitude_db(sr, freq);
    }
    return db;
}

// The preamp that just stops the curve clipping: minus its highest peak, and
// never positive. This is exactly what AutoEq ships in its Preamp: line, so a
// downloaded profile arrives with the right value already set.
inline float eq_suggest_preamp(const EqProfile& prof)
{
    EqProfile flat = prof;
    flat.preamp = 0.0f;
    float peak = 0.0f;
    for (int i = 0; i <= 480; ++i) {
        const float f = 20.0f * std::pow(1000.0f, i / 480.0f);   // 20 Hz .. 20 kHz
        peak = std::max(peak, eq_response_db(flat, f));
    }
    return peak <= 0.0f ? 0.0f : -std::ceil(peak * 10.0f) / 10.0f;
}

// ---------------------------------------------------------------------------
// Moving a profile on and off a live bus.
// ---------------------------------------------------------------------------

// Trims a profile to the bands the hardware has. Profiles with more bands than
// kBusEqBands keep the ones that change the sound most, rather than whichever
// happen to come first: shelves and pass filters shape the whole curve, so they
// always survive, and the remaining slots go to the largest boosts and cuts.
inline std::vector<EqBand> eq_fit_bands(const std::vector<EqBand>& in)
{
    if ((int)in.size() <= kBusEqBands) return in;
    std::vector<int> idx(in.size());
    for (size_t i = 0; i < in.size(); ++i) idx[i] = (int)i;
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const bool wa = !eq_type_uses_gain(in[a].type), wb = !eq_type_uses_gain(in[b].type);
        if (wa != wb) return wa;
        return std::fabs(in[a].gain) > std::fabs(in[b].gain);
    });
    idx.resize(kBusEqBands);
    std::sort(idx.begin(), idx.end());
    std::vector<EqBand> out;
    out.reserve(idx.size());
    for (int i : idx) out.push_back(in[i]);
    return out;
}

// Writes a profile onto a bus. Unused bands are reset to flat peaks rather than
// left holding whatever the previous profile put there.
inline void eq_apply_to_bus(Shared* s, int bus, const EqProfile& prof)
{
    if (!s || bus < 0 || bus >= kBuses) return;
    BusParams& p = s->bus[bus];
    const std::vector<EqBand> bands = eq_fit_bands(prof.bands);

    static const float kSpread[kBusEqBands] = {
        31, 62, 125, 250, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000
    };
    for (int k = 0; k < kBusEqBands; ++k) {
        if (k < (int)bands.size()) {
            const EqBand& b = bands[k];
            p.eq_freq[k].store(clampf(b.freq, 10.0f, 24000.0f));
            p.eq_gain[k].store(clampf(b.gain, -24.0f, 24.0f));
            p.eq_q[k].store(clampf(b.q, 0.1f, 20.0f));
            p.eq_type[k].store(b.type);
            p.eq_band_on[k].store(b.on ? 1 : 0);
        } else {
            p.eq_freq[k].store(kSpread[k]);
            p.eq_gain[k].store(0.0f);
            p.eq_q[k].store(1.0f);
            p.eq_type[k].store(kEqPeak);
            p.eq_band_on[k].store(1);
        }
    }
    p.eq_preamp_db.store(clampf(prof.preamp, -24.0f, 12.0f));
}

inline EqProfile eq_capture_from_bus(const Shared* s, int bus, const std::string& name = "")
{
    EqProfile prof;
    prof.name = name;
    if (!s || bus < 0 || bus >= kBuses) return prof;
    const BusParams& p = s->bus[bus];
    prof.preamp = p.eq_preamp_db.load();
    for (int k = 0; k < kBusEqBands; ++k) {
        EqBand b;
        b.freq = p.eq_freq[k].load();
        b.gain = p.eq_gain[k].load();
        b.q    = p.eq_q[k].load();
        b.type = p.eq_type[k].load();
        b.on   = p.eq_band_on[k].load() != 0;
        prof.bands.push_back(b);
    }
    return prof;
}

// ---------------------------------------------------------------------------
// Files. User profiles live one per file next to the mixer presets, named after
// the profile, so they can be listed, copied and edited by hand.
// ---------------------------------------------------------------------------
inline std::string eq_profile_dir() { return preset_dir() + "/eq"; }

inline bool eq_write_file(const EqProfile& prof, const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const std::string text = eq_format_apo(prof);
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

inline bool eq_read_file(EqProfile& prof, const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    prof = eq_parse_apo(text);
    return !prof.bands.empty() || prof.preamp != 0.0f;
}

// ---------------------------------------------------------------------------
// Built-in starting points. These are deliberately gentle and generic: they are
// somewhere to start from, not a substitute for a measured headphone profile.
// ---------------------------------------------------------------------------
struct EqFactoryPreset {
    const char* name;
    const char* text;
};

inline const std::vector<EqFactoryPreset>& eq_factory_presets()
{
    static const std::vector<EqFactoryPreset> kPresets = {
        { "Flat",
          "Preamp: 0.0 dB\n" },

        { "Bass Boost",
          "Preamp: -8.3 dB\n"
          "Filter 1: ON LSC Fc 105 Hz Gain 6.0 dB Q 0.70\n"
          "Filter 2: ON PK Fc 45 Hz Gain 2.5 dB Q 0.90\n" },

        { "Bass Boost (heavy)",
          "Preamp: -13.2 dB\n"
          "Filter 1: ON LSC Fc 120 Hz Gain 10.0 dB Q 0.70\n"
          "Filter 2: ON PK Fc 50 Hz Gain 3.5 dB Q 0.80\n"
          "Filter 3: ON PK Fc 320 Hz Gain -2.0 dB Q 1.20\n" },

        { "Bass Cut",
          "Preamp: 0.0 dB\n"
          "Filter 1: ON HPQ Fc 80 Hz Q 0.70\n"
          "Filter 2: ON LSC Fc 150 Hz Gain -4.0 dB Q 0.70\n" },

        { "Treble Boost",
          "Preamp: -5.0 dB\n"
          "Filter 1: ON HSC Fc 6000 Hz Gain 5.0 dB Q 0.70\n" },

        { "Vocal Clarity",
          "Preamp: -3.8 dB\n"
          "Filter 1: ON PK Fc 250 Hz Gain -3.0 dB Q 1.00\n"
          "Filter 2: ON PK Fc 2500 Hz Gain 3.5 dB Q 1.20\n"
          "Filter 3: ON PK Fc 5000 Hz Gain 2.0 dB Q 1.50\n" },

        { "Loudness (smiley)",
          "Preamp: -5.5 dB\n"
          "Filter 1: ON LSC Fc 100 Hz Gain 5.5 dB Q 0.70\n"
          "Filter 2: ON PK Fc 900 Hz Gain -2.5 dB Q 1.00\n"
          "Filter 3: ON HSC Fc 7000 Hz Gain 4.5 dB Q 0.70\n" },

        { "Speech / Podcast",
          "Preamp: -3.0 dB\n"
          "Filter 1: ON HPQ Fc 90 Hz Q 0.70\n"
          "Filter 2: ON PK Fc 200 Hz Gain -2.5 dB Q 1.00\n"
          "Filter 3: ON PK Fc 3000 Hz Gain 3.0 dB Q 1.00\n"
          "Filter 4: ON LPQ Fc 14000 Hz Q 0.70\n" },

        { "Gaming (footsteps)",
          "Preamp: -5.2 dB\n"
          "Filter 1: ON LSC Fc 120 Hz Gain -3.0 dB Q 0.70\n"
          "Filter 2: ON PK Fc 1800 Hz Gain 2.5 dB Q 1.40\n"
          "Filter 3: ON PK Fc 4500 Hz Gain 4.5 dB Q 1.20\n"
          "Filter 4: ON PK Fc 8000 Hz Gain 2.0 dB Q 1.50\n" },

        { "De-harsh",
          "Preamp: 0.0 dB\n"
          "Filter 1: ON PK Fc 3200 Hz Gain -3.0 dB Q 2.00\n"
          "Filter 2: ON PK Fc 6300 Hz Gain -3.5 dB Q 2.50\n" },

        { "Warm",
          "Preamp: -3.5 dB\n"
          "Filter 1: ON LSC Fc 200 Hz Gain 3.5 dB Q 0.70\n"
          "Filter 2: ON HSC Fc 8000 Hz Gain -2.5 dB Q 0.70\n" },

        { "Small Speakers",
          "Preamp: -3.2 dB\n"
          "Filter 1: ON HPQ Fc 120 Hz Q 0.70\n"
          "Filter 2: ON PK Fc 220 Hz Gain 3.5 dB Q 1.00\n"
          "Filter 3: ON PK Fc 3500 Hz Gain 2.0 dB Q 1.20\n" },
    };
    return kPresets;
}

inline bool eq_factory_profile(const std::string& name, EqProfile& out)
{
    for (const EqFactoryPreset& p : eq_factory_presets()) {
        if (name == p.name) {
            out = eq_parse_apo(p.text);
            out.name = p.name;
            return true;
        }
    }
    return false;
}

} // namespace bb
