// betterbanana - preset save/load.
//
// A preset is the whole mixer state as plain text: one "key value" per line,
// value being everything after the first space (device names may contain
// almost anything, so no further splitting). Header-only so the engine, the
// GUI and bb-ctl all read and write exactly the same thing.
//
// The text form is the unit of undo as well as the unit of storage, so
// serialize/deserialize are the primitives and the file functions wrap them.
#pragma once

#include "protocol.h"

#include <unistd.h>
#include <sys/stat.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bb {

// 3 added the per-strip parametric EQ, and the two strip fields (mono-source
// fold, limiter ceiling) that had never been written. 2 added the per-band
// filter type / bypass flag and the bus EQ preamp. Both still load: every
// field a file omits is reset to its default rather than left over.
constexpr int kPresetVersion = 3;

inline std::string preset_dir()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (std::string(home ? home : ".") + "/.config");
    return base + "/betterbanana";
}
inline std::string presets_path()  { return preset_dir() + "/presets"; }

// ---------------------------------------------------------------------------
// The startup preset.
//
// The engine used to save the session on exit and reload it on start. It does
// not any more: presets are explicit, and exactly one of them - named here -
// is loaded when the engine comes up. The marker holds either a bare preset
// name or an absolute path.
// ---------------------------------------------------------------------------
inline std::string startup_marker_path() { return preset_dir() + "/startup"; }

// Where a preset called `name` lives. Anything with a slash is a literal path.
inline std::string preset_path_for(const std::string& name)
{
    if (name.empty()) return std::string();
    if (name.find('/') != std::string::npos) return name;
    return presets_path() + "/" + name + ".bbp";
}

// The name recorded in the marker, or empty if none is set.
inline std::string startup_preset_name()
{
    FILE* f = fopen(startup_marker_path().c_str(), "r");
    if (!f) return std::string();
    char buf[512] = {};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return std::string(); }
    fclose(f);
    std::string v(buf);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

// Empty clears the choice - but writes the marker anyway, empty. "Start with a
// default mixer" is a deliberate answer and must not read as "never asked": the
// one-off migration below keys off the marker existing, and deleting it would
// let a stale autosave.bbp come back and re-set the very thing just cleared.
inline bool set_startup_preset_name(const std::string& name)
{
    mkdir(preset_dir().c_str(), 0755);
    FILE* f = fopen(startup_marker_path().c_str(), "w");
    if (!f) return false;
    if (!name.empty()) fprintf(f, "%s\n", name.c_str());
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
namespace detail {

inline void addf(std::string& out, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

inline void write_eq(std::string& out, const char* prefix, int idx, const EqParams& p)
{
    addf(out, "%s.%d.eqon %d\n",   prefix, idx, p.on.load());
    addf(out, "%s.%d.preamp %.3f\n", prefix, idx, p.preamp_db.load());
    for (int k = 0; k < kEqBands; ++k)
        addf(out, "%s.%d.band.%d %.3f %.3f %.3f %d %d\n", prefix, idx, k,
             p.gain[k].load(), p.freq[k].load(), p.q[k].load(),
             p.type[k].load(), p.band_on[k].load());
}

// One band line, shared by every EQ block the format carries.
inline bool read_band(EqParams& p, int k, const char* val)
{
    if (k < 0 || k >= kEqBands) return false;
    float g = 0, f = 0, q = 0;
    int type = kEqPeak, on = 1;
    const int got = sscanf(val, "%f %f %f %d %d", &g, &f, &q, &type, &on);
    if (got < 3) return false;
    p.gain[k].store(g);
    p.freq[k].store(f);
    p.q[k].store(q);
    p.type[k].store(got >= 4 && type >= 0 && type < kEqTypeCount ? type : kEqPeak);
    p.band_on[k].store(got >= 5 ? (on ? 1 : 0) : 1);
    return true;
}

// Anything the file did not mention goes back to its default, so a preset
// written by an older version fully determines the state rather than leaving
// the tail of whatever curve happened to be loaded before.
inline void reset_unseen(EqParams& p, const bool* seen, bool preamp_seen)
{
    for (int k = 0; k < kEqBands; ++k) {
        if (seen[k]) continue;
        p.gain[k].store(0.0f);
        p.freq[k].store(kEqDefaultFreq[k]);
        p.q[k].store(1.0f);
        p.type[k].store(kEqPeak);
        p.band_on[k].store(1);
    }
    if (!preamp_seen) p.preamp_db.store(0.0f);
}

} // namespace detail

// ---------------------------------------------------------------------------
inline std::string preset_serialize(const Shared* s)
{
    std::string out;
    out.reserve(16384);
    detail::addf(out, "betterbanana-preset %d\n", kPresetVersion);

    char hw[kHwStrips][kNameLen], bo[kPhysBuses][kNameLen];
    char hwd[kHwStrips][kNameLen], bod[kPhysBuses][kNameLen];
    uint32_t seq = 0;
    bool got = false;
    for (int t = 0; t < 16 && !got; ++t)
        got = routing_read(s->routing, seq, hw, bo, hwd, bod);
    if (!got) {
        std::memset(hw, 0, sizeof(hw)); std::memset(bo, 0, sizeof(bo));
        std::memset(hwd, 0, sizeof(hwd)); std::memset(bod, 0, sizeof(bod));
    }

    for (int i = 0; i < kStrips; ++i) {
        const StripParams& p = s->strip[i];
        detail::addf(out, "strip.%d.gain %.3f\n", i, p.gain_db.load());
        detail::addf(out, "strip.%d.mute %d\n", i, p.mute.load());
        detail::addf(out, "strip.%d.solo %d\n", i, p.solo.load());
        detail::addf(out, "strip.%d.mono %d\n", i, p.mono.load());
        detail::addf(out, "strip.%d.monosrc %d\n", i, p.mono_source.load());
        detail::addf(out, "strip.%d.limit %.3f\n", i, p.limit_db.load());
        detail::addf(out, "strip.%d.gate %.3f\n", i, p.gate.load());
        detail::addf(out, "strip.%d.comp %.3f\n", i, p.comp.load());
        detail::addf(out, "strip.%d.aud %.3f\n", i, p.audibility.load());
        detail::addf(out, "strip.%d.eq %.3f %.3f %.3f\n", i,
                     p.eq_low.load(), p.eq_mid.load(), p.eq_high.load());
        detail::addf(out, "strip.%d.pan %.4f %.4f\n", i, p.pan_x.load(), p.pan_y.load());
        detail::addf(out, "strip.%d.buses", i);
        for (int b = 0; b < kBuses; ++b) detail::addf(out, " %d", p.bus_on[b].load());
        out += "\n";
        detail::addf(out, "strip.%d.duck %d %.3f\n", i, p.duck_key.load(), p.duck_depth_db.load());
        detail::write_eq(out, "strip", i, p.eq);
        if (i < kHwStrips) {
            detail::addf(out, "strip.%d.device %s\n", i, hw[i]);
            if (hwd[i][0]) detail::addf(out, "strip.%d.devicedesc %s\n", i, hwd[i]);
        }
    }

    for (int b = 0; b < kBuses; ++b) {
        const BusParams& p = s->bus[b];
        detail::addf(out, "bus.%d.gain %.3f\n", b, p.gain_db.load());
        detail::addf(out, "bus.%d.mute %d\n", b, p.mute.load());
        detail::addf(out, "bus.%d.mono %d\n", b, p.mono.load());
        // "bus.N.eq" is the v1/v2 spelling of the block's on/off flag; it stays
        // so an old preset and a new one mean the same thing.
        detail::addf(out, "bus.%d.eq %d\n", b, p.eq.on.load());
        detail::addf(out, "bus.%d.preamp %.3f\n", b, p.eq.preamp_db.load());
        for (int k = 0; k < kEqBands; ++k)
            detail::addf(out, "bus.%d.band.%d %.3f %.3f %.3f %d %d\n", b, k,
                         p.eq.gain[k].load(), p.eq.freq[k].load(), p.eq.q[k].load(),
                         p.eq.type[k].load(), p.eq.band_on[k].load());
        if (b < kPhysBuses) {
            detail::addf(out, "bus.%d.device %s\n", b, bo[b]);
            if (bod[b][0]) detail::addf(out, "bus.%d.devicedesc %s\n", b, bod[b]);
        }
    }

    detail::addf(out, "duck %d %.3f %.3f %.3f\n", s->duck_enabled.load(),
                 s->duck_threshold_db.load(), s->duck_attack_ms.load(),
                 s->duck_release_ms.load());
    detail::addf(out, "rec.source_bus %d\n", s->rec.source_bus.load());
    detail::addf(out, "rec.gain %.3f\n", s->rec.gain_db.load());
    detail::addf(out, "rec.loop %d\n", s->rec.loop.load());
    out += "rec.buses";
    for (int b = 0; b < kBuses; ++b) detail::addf(out, " %d", s->rec.bus_on[b].load());
    out += "\n";

    char lstrip[kStrips][kLabelLen], lbus[kBuses][kLabelLen];
    bool lok = false;
    for (int t = 0; t < 16 && !lok; ++t) lok = labels_read(s->labels, lstrip, lbus);
    if (lok) {
        for (int i = 0; i < kStrips; ++i)
            if (lstrip[i][0]) detail::addf(out, "label.strip.%d %s\n", i, lstrip[i]);
        for (int b = 0; b < kBuses; ++b)
            if (lbus[b][0]) detail::addf(out, "label.bus.%d %s\n", b, lbus[b]);
    }

    for (int i = 0; i < kVbanStreams; ++i) {
        const VbanOutCfg& o = s->vban.out[i];
        detail::addf(out, "vban.out.%d %d %d %d %d %s\n", i, o.enabled, o.source_bus,
                     o.port, o.rate, o.name);
        detail::addf(out, "vban.out.%d.host %s\n", i, o.host);
        const VbanInCfg& n = s->vban.in[i];
        detail::addf(out, "vban.in.%d %d %d %d %s\n", i, n.enabled, n.port, n.rate, n.name);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Applies preset text onto the live shared state. Bumps the seqlocks so the
// engine picks up device assignment and VBAN changes on its next control poll.
inline bool preset_deserialize(Shared* s, const std::string& text)
{
    if (!s) return false;
    int ver = 0;
    if (sscanf(text.c_str(), "betterbanana-preset %d", &ver) != 1 || ver > kPresetVersion)
        return false;

    char hw[kHwStrips][kNameLen] = {}, out[kPhysBuses][kNameLen] = {};
    char hwd[kHwStrips][kNameLen] = {}, outd[kPhysBuses][kNameLen] = {};
    bool touched_routing = false;

    bool bus_band_seen[kBuses][kEqBands] = {}, bus_eq_seen[kBuses] = {}, bus_pre_seen[kBuses] = {};
    bool str_band_seen[kStrips][kEqBands] = {}, str_eq_seen[kStrips] = {}, str_pre_seen[kStrips] = {};

    // VBAN entries and labels are written as they are parsed, so keep those
    // seqlocks held open for the whole pass; a reader simply retries.
    s->vban.seq.fetch_add(1, std::memory_order_acq_rel);
    s->labels.seq.fetch_add(1, std::memory_order_acq_rel);
    std::memset(s->labels.strip, 0, sizeof(s->labels.strip));
    std::memset(s->labels.bus,   0, sizeof(s->labels.bus));

    size_t pos = text.find('\n');
    pos = pos == std::string::npos ? text.size() : pos + 1;

    char line[512];
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const size_t len = nl - pos;
        if (len == 0 || len >= sizeof(line)) { pos = nl + 1; continue; }
        std::memcpy(line, text.data() + pos, len);
        line[len] = 0;
        pos = nl + 1;

        size_t l = len;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        char* sp = std::strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char* key = line;
        const char* val = sp + 1;

        // sscanf reports assigned fields, not literal matches, so
        // sscanf("strip.0.mute", "strip.%d.gain", &i) also returns 1. Match
        // keys exactly instead: fixed prefix, integer, fixed suffix.
        auto keyed = [&](const char* prefix, const char* suffix, int& idx) {
            const size_t plen = std::strlen(prefix);
            if (std::strncmp(key, prefix, plen) != 0) return false;
            const char* q = key + plen;
            char* endp = nullptr;
            const long v = std::strtol(q, &endp, 10);
            if (endp == q || std::strcmp(endp, suffix) != 0) return false;
            idx = (int)v;
            return true;
        };
        auto keyed2 = [&](const char* prefix, const char* mid, const char* suffix,
                          int& idx, int& jdx) {
            const size_t plen = std::strlen(prefix);
            if (std::strncmp(key, prefix, plen) != 0) return false;
            const char* q = key + plen;
            char* endp = nullptr;
            const long v = std::strtol(q, &endp, 10);
            if (endp == q) return false;
            const size_t mlen = std::strlen(mid);
            if (std::strncmp(endp, mid, mlen) != 0) return false;
            const char* r = endp + mlen;
            char* end2 = nullptr;
            const long w = std::strtol(r, &end2, 10);
            if (end2 == r || std::strcmp(end2, suffix) != 0) return false;
            idx = (int)v; jdx = (int)w;
            return true;
        };

        int i = 0, k = 0;
        float a = 0, b = 0, c = 0;
        int ia = 0, ib = 0, ic = 0, id = 0;
        char name[64] = {};

        if      (keyed("strip.", ".gain", i) && i < kStrips) s->strip[i].gain_db.store(atof(val));
        else if (keyed("strip.", ".mute", i) && i < kStrips) s->strip[i].mute.store(atoi(val));
        else if (keyed("strip.", ".solo", i) && i < kStrips) s->strip[i].solo.store(atoi(val));
        else if (keyed("strip.", ".monosrc", i) && i < kStrips) s->strip[i].mono_source.store(atoi(val) ? 1 : 0);
        else if (keyed("strip.", ".mono", i) && i < kStrips) s->strip[i].mono.store(atoi(val));
        else if (keyed("strip.", ".limit", i) && i < kStrips) s->strip[i].limit_db.store(atof(val));
        else if (keyed("strip.", ".gate", i) && i < kStrips) s->strip[i].gate.store(atof(val));
        else if (keyed("strip.", ".comp", i) && i < kStrips) s->strip[i].comp.store(atof(val));
        else if (keyed("strip.", ".aud",  i) && i < kStrips) s->strip[i].audibility.store(atof(val));
        else if (keyed("strip.", ".eqon", i) && i < kStrips) {
            s->strip[i].eq.on.store(atoi(val) ? 1 : 0);
            str_eq_seen[i] = true;
        }
        else if (keyed("strip.", ".preamp", i) && i < kStrips) {
            s->strip[i].eq.preamp_db.store(atof(val));
            str_pre_seen[i] = true;
            str_eq_seen[i] = true;
        }
        else if (keyed2("strip.", ".band.", "", i, k) && i < kStrips) {
            if (detail::read_band(s->strip[i].eq, k, val)) {
                str_band_seen[i][k] = true;
                str_eq_seen[i] = true;
            }
        }
        else if (keyed("strip.", ".eq",   i) && i < kStrips) {
            if (sscanf(val, "%f %f %f", &a, &b, &c) == 3) {
                s->strip[i].eq_low.store(a); s->strip[i].eq_mid.store(b); s->strip[i].eq_high.store(c);
            }
        }
        else if (keyed("strip.", ".pan", i) && i < kStrips) {
            if (sscanf(val, "%f %f", &a, &b) == 2) {
                s->strip[i].pan_x.store(a); s->strip[i].pan_y.store(b);
            }
        }
        else if (keyed("strip.", ".buses", i) && i < kStrips) {
            int v[kBuses] = {};
            const int got = sscanf(val, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]);
            for (int x = 0; x < got && x < kBuses; ++x) s->strip[i].bus_on[x].store(v[x] ? 1 : 0);
        }
        else if (keyed("strip.", ".devicedesc", i) && i < kHwStrips) {
            snprintf(hwd[i], kNameLen, "%s", val); touched_routing = true;
        }
        else if (keyed("strip.", ".device", i) && i < kHwStrips) {
            snprintf(hw[i], kNameLen, "%s", val); touched_routing = true;
        }
        else if (keyed("strip.", ".duck", i) && i < kStrips) {
            int on = 0; float depth = 0;
            if (sscanf(val, "%d %f", &on, &depth) == 2) {
                s->strip[i].duck_key.store(on);
                s->strip[i].duck_depth_db.store(depth);
            }
        }
        else if (keyed2("bus.", ".band.", "", i, k) && i < kBuses) {
            if (detail::read_band(s->bus[i].eq, k, val)) {
                bus_band_seen[i][k] = true;
                bus_eq_seen[i] = true;
            }
        }
        else if (keyed("bus.", ".preamp", i) && i < kBuses) {
            s->bus[i].eq.preamp_db.store(atof(val));
            bus_pre_seen[i] = true;
            bus_eq_seen[i] = true;
        }
        else if (keyed("bus.", ".gain", i) && i < kBuses) s->bus[i].gain_db.store(atof(val));
        else if (keyed("bus.", ".mute", i) && i < kBuses) s->bus[i].mute.store(atoi(val));
        else if (keyed("bus.", ".mono", i) && i < kBuses) s->bus[i].mono.store(atoi(val));
        else if (keyed("bus.", ".eq",   i) && i < kBuses) {
            s->bus[i].eq.on.store(atoi(val) ? 1 : 0);
            bus_eq_seen[i] = true;
        }
        else if (keyed("bus.", ".devicedesc", i) && i < kPhysBuses) {
            snprintf(outd[i], kNameLen, "%s", val); touched_routing = true;
        }
        else if (keyed("bus.", ".device", i) && i < kPhysBuses) {
            snprintf(out[i], kNameLen, "%s", val); touched_routing = true;
        }
        else if (!std::strcmp(key, "duck")) {
            int on = 0; float thr = 0, at = 0, rel = 0;
            if (sscanf(val, "%d %f %f %f", &on, &thr, &at, &rel) == 4) {
                s->duck_enabled.store(on);
                s->duck_threshold_db.store(thr);
                s->duck_attack_ms.store(at);
                s->duck_release_ms.store(rel);
            }
        }
        else if (!std::strcmp(key, "rec.source_bus")) s->rec.source_bus.store(atoi(val));
        else if (!std::strcmp(key, "rec.gain"))       s->rec.gain_db.store(atof(val));
        else if (!std::strcmp(key, "rec.loop"))       s->rec.loop.store(atoi(val));
        else if (!std::strcmp(key, "rec.buses")) {
            int v[kBuses] = {};
            const int got = sscanf(val, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]);
            for (int x = 0; x < got && x < kBuses; ++x) s->rec.bus_on[x].store(v[x] ? 1 : 0);
        }
        else if (keyed("label.strip.", "", i) && i < kStrips) {
            snprintf(s->labels.strip[i], kLabelLen, "%s", val);
        }
        else if (keyed("label.bus.", "", i) && i < kBuses) {
            snprintf(s->labels.bus[i], kLabelLen, "%s", val);
        }
        else if (keyed("vban.out.", ".host", i) && i < kVbanStreams) {
            snprintf(s->vban.out[i].host, sizeof(s->vban.out[i].host), "%s", val);
        }
        else if (keyed("vban.out.", "", i) && i < kVbanStreams) {
            if (sscanf(val, "%d %d %d %d %63s", &ia, &ib, &ic, &id, name) >= 4) {
                VbanOutCfg& o = s->vban.out[i];
                o.enabled = ia; o.source_bus = ib; o.port = ic; o.rate = id; o.channels = 2;
                if (name[0]) snprintf(o.name, sizeof(o.name), "%s", name);
            }
        }
        else if (keyed("vban.in.", "", i) && i < kVbanStreams) {
            if (sscanf(val, "%d %d %d %63s", &ia, &ib, &ic, name) >= 3) {
                VbanInCfg& n = s->vban.in[i];
                n.enabled = ia; n.port = ib; n.rate = ic; n.channels = 2;
                if (name[0]) snprintf(n.name, sizeof(n.name), "%s", name);
            }
        }
    }

    for (int b = 0; b < kBuses; ++b)
        if (bus_eq_seen[b]) detail::reset_unseen(s->bus[b].eq, bus_band_seen[b], bus_pre_seen[b]);
    // A preset written before v3 describes no strip EQ at all, so every strip
    // block goes back to flat rather than keeping whatever was there.
    for (int i = 0; i < kStrips; ++i) {
        if (str_eq_seen[i]) detail::reset_unseen(s->strip[i].eq, str_band_seen[i], str_pre_seen[i]);
        else                eq_set_defaults(s->strip[i].eq);
    }

    if (touched_routing) {
        routing_write_begin(s->routing);
        std::memcpy(s->routing.hw_in,        hw,   sizeof(hw));
        std::memcpy(s->routing.bus_out,      out,  sizeof(out));
        std::memcpy(s->routing.hw_in_desc,   hwd,  sizeof(hwd));
        std::memcpy(s->routing.bus_out_desc, outd, sizeof(outd));
        routing_write_end(s->routing);
    }
    s->vban.seq.fetch_add(1, std::memory_order_release);
    s->labels.seq.fetch_add(1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Files. Written through a temporary and renamed into place: a crash or a full
// disk part-way through must not leave a truncated preset that silently comes
// up as a half-configured mixer.
// ---------------------------------------------------------------------------
inline bool write_file_atomic(const char* path, const std::string& text)
{
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool flushed = wrote && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!flushed) { unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

inline bool read_file(const char* path, std::string& text)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    text.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return true;
}

inline bool save_preset(const Shared* s, const char* path)
{
    return write_file_atomic(path, preset_serialize(s));
}

inline bool load_preset(Shared* s, const char* path)
{
    std::string text;
    if (!read_file(path, text)) return false;
    return preset_deserialize(s, text);
}

// ---------------------------------------------------------------------------
// Per-device strip settings.
//
// Opt-in only: nothing is written unless you ask for it. Once a device has a
// snapshot, pointing a strip at that device restores the processing you set up
// for it - the microphone's gate, compressor, EQ and level travel with the
// microphone, the way a bus's headphone correction travels with the headphones.
// Bus assignment is deliberately NOT included: which buses a strip feeds is a
// property of the mix, not of what is plugged in.
// ---------------------------------------------------------------------------
inline std::string path_escape(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_') {
            out += (char)ch;
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

inline std::string device_dir() { return preset_dir() + "/devices"; }

inline std::string device_strip_path(const std::string& node)
{
    return device_dir() + "/" + path_escape(node) + ".strip";
}

inline std::string strip_serialize(const StripParams& p)
{
    std::string out;
    detail::addf(out, "betterbanana-strip %d\n", kPresetVersion);
    detail::addf(out, "gain %.3f\n", p.gain_db.load());
    detail::addf(out, "mute %d\n", p.mute.load());
    detail::addf(out, "solo %d\n", p.solo.load());
    detail::addf(out, "mono %d\n", p.mono.load());
    detail::addf(out, "monosrc %d\n", p.mono_source.load());
    detail::addf(out, "limit %.3f\n", p.limit_db.load());
    detail::addf(out, "gate %.3f\n", p.gate.load());
    detail::addf(out, "comp %.3f\n", p.comp.load());
    detail::addf(out, "aud %.3f\n", p.audibility.load());
    detail::addf(out, "tone %.3f %.3f %.3f\n",
                 p.eq_low.load(), p.eq_mid.load(), p.eq_high.load());
    detail::addf(out, "pan %.4f %.4f\n", p.pan_x.load(), p.pan_y.load());
    detail::addf(out, "duck %d %.3f\n", p.duck_key.load(), p.duck_depth_db.load());
    detail::addf(out, "eqon %d\n", p.eq.on.load());
    detail::addf(out, "preamp %.3f\n", p.eq.preamp_db.load());
    for (int k = 0; k < kEqBands; ++k)
        detail::addf(out, "band.%d %.3f %.3f %.3f %d %d\n", k,
                     p.eq.gain[k].load(), p.eq.freq[k].load(), p.eq.q[k].load(),
                     p.eq.type[k].load(), p.eq.band_on[k].load());
    return out;
}

inline bool strip_deserialize(StripParams& p, const std::string& text)
{
    int ver = 0;
    if (sscanf(text.c_str(), "betterbanana-strip %d", &ver) != 1 || ver > kPresetVersion)
        return false;

    bool band_seen[kEqBands] = {}, preamp_seen = false;
    size_t pos = text.find('\n');
    pos = pos == std::string::npos ? text.size() : pos + 1;

    char line[512];
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const size_t len = nl - pos;
        if (len == 0 || len >= sizeof(line)) { pos = nl + 1; continue; }
        std::memcpy(line, text.data() + pos, len);
        line[len] = 0;
        pos = nl + 1;

        char* sp = std::strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char* key = line;
        const char* val = sp + 1;
        float a = 0, b = 0, c = 0;

        if      (!std::strcmp(key, "gain"))    p.gain_db.store(atof(val));
        else if (!std::strcmp(key, "mute"))    p.mute.store(atoi(val) ? 1 : 0);
        else if (!std::strcmp(key, "solo"))    p.solo.store(atoi(val) ? 1 : 0);
        else if (!std::strcmp(key, "mono"))    p.mono.store(atoi(val) ? 1 : 0);
        else if (!std::strcmp(key, "monosrc")) p.mono_source.store(atoi(val) ? 1 : 0);
        else if (!std::strcmp(key, "limit"))   p.limit_db.store(atof(val));
        else if (!std::strcmp(key, "gate"))    p.gate.store(atof(val));
        else if (!std::strcmp(key, "comp"))    p.comp.store(atof(val));
        else if (!std::strcmp(key, "aud"))     p.audibility.store(atof(val));
        else if (!std::strcmp(key, "eqon"))    p.eq.on.store(atoi(val) ? 1 : 0);
        else if (!std::strcmp(key, "preamp"))  { p.eq.preamp_db.store(atof(val)); preamp_seen = true; }
        else if (!std::strcmp(key, "tone")) {
            if (sscanf(val, "%f %f %f", &a, &b, &c) == 3) {
                p.eq_low.store(a); p.eq_mid.store(b); p.eq_high.store(c);
            }
        }
        else if (!std::strcmp(key, "pan")) {
            if (sscanf(val, "%f %f", &a, &b) == 2) { p.pan_x.store(a); p.pan_y.store(b); }
        }
        else if (!std::strcmp(key, "duck")) {
            int on = 0; float depth = 0;
            if (sscanf(val, "%d %f", &on, &depth) == 2) {
                p.duck_key.store(on); p.duck_depth_db.store(depth);
            }
        }
        else if (!std::strncmp(key, "band.", 5)) {
            const int k = atoi(key + 5);
            if (detail::read_band(p.eq, k, val)) band_seen[k] = true;
        }
    }
    detail::reset_unseen(p.eq, band_seen, preamp_seen);
    return true;
}

inline bool has_strip_for_device(const std::string& node)
{
    if (node.empty()) return false;
    return access(device_strip_path(node).c_str(), R_OK) == 0;
}

inline bool save_strip_for_device(const Shared* s, int strip, const std::string& node)
{
    if (!s || node.empty() || strip < 0 || strip >= kStrips) return false;
    mkdir(preset_dir().c_str(), 0755);
    mkdir(device_dir().c_str(), 0755);
    return write_file_atomic(device_strip_path(node).c_str(),
                             strip_serialize(s->strip[strip]));
}

inline bool load_strip_for_device(Shared* s, int strip, const std::string& node)
{
    if (!s || node.empty() || strip < 0 || strip >= kStrips) return false;
    std::string text;
    if (!read_file(device_strip_path(node).c_str(), text)) return false;
    return strip_deserialize(s->strip[strip], text);
}

inline bool forget_strip_for_device(const std::string& node)
{
    if (node.empty()) return false;
    return unlink(device_strip_path(node).c_str()) == 0;
}

// ---------------------------------------------------------------------------
// One-off migration off the old behaviour, where the engine saved the session
// on exit and reloaded it on start. Presets are explicit now, so the last
// automatic save becomes an ordinary preset and the startup choice: upgrading
// must not silently come up with a default mixer. Runs once - afterwards a
// startup marker exists, and the old file is kept under a new name.
// ---------------------------------------------------------------------------
inline bool migrate_autosave(std::string* saved_as = nullptr)
{
    const std::string old = preset_dir() + "/autosave.bbp";
    if (access(startup_marker_path().c_str(), F_OK) == 0) return false;
    std::string text;
    if (!read_file(old.c_str(), text) || text.empty()) return false;

    const std::string name = "Previous session";
    mkdir(preset_dir().c_str(), 0755);
    mkdir(presets_path().c_str(), 0755);
    if (!write_file_atomic(preset_path_for(name).c_str(), text)) return false;
    if (!set_startup_preset_name(name)) return false;
    rename(old.c_str(), (old + ".migrated").c_str());
    if (saved_as) *saved_as = name;
    return true;
}

} // namespace bb
