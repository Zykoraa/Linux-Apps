// betterbanana - preset save/load.
//
// A preset is the whole mixer state as plain text: one "key value" per line,
// value being everything after the first space (device names may contain
// almost anything, so no further splitting). Header-only so the engine, the
// GUI and bb-ctl all read and write exactly the same thing.
#pragma once

#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bb {

// 2 added the per-band filter type / bypass flag and the EQ preamp. Version 1
// files still load: the extra fields are optional on the band line.
constexpr int kPresetVersion = 2;

inline std::string preset_dir()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (std::string(home ? home : ".") + "/.config");
    return base + "/betterbanana";
}
inline std::string autosave_path() { return preset_dir() + "/autosave.bbp"; }
inline std::string presets_path()  { return preset_dir() + "/presets"; }

// ---------------------------------------------------------------------------
inline bool save_preset(const Shared* s, const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "betterbanana-preset %d\n", kPresetVersion);

    char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
    char hwd[kHwStrips][kNameLen], outd[kPhysBuses][kNameLen];
    uint32_t seq = 0;
    bool got = false;
    for (int t = 0; t < 16 && !got; ++t)
        got = routing_read(s->routing, seq, hw, out, hwd, outd);
    if (!got) {
        std::memset(hw, 0, sizeof(hw)); std::memset(out, 0, sizeof(out));
        std::memset(hwd, 0, sizeof(hwd)); std::memset(outd, 0, sizeof(outd));
    }

    for (int i = 0; i < kStrips; ++i) {
        const StripParams& p = s->strip[i];
        fprintf(f, "strip.%d.gain %.3f\n", i, p.gain_db.load());
        fprintf(f, "strip.%d.mute %d\n", i, p.mute.load());
        fprintf(f, "strip.%d.solo %d\n", i, p.solo.load());
        fprintf(f, "strip.%d.mono %d\n", i, p.mono.load());
        fprintf(f, "strip.%d.gate %.3f\n", i, p.gate.load());
        fprintf(f, "strip.%d.comp %.3f\n", i, p.comp.load());
        fprintf(f, "strip.%d.aud %.3f\n", i, p.audibility.load());
        fprintf(f, "strip.%d.eq %.3f %.3f %.3f\n", i,
                p.eq_low.load(), p.eq_mid.load(), p.eq_high.load());
        fprintf(f, "strip.%d.pan %.4f %.4f\n", i, p.pan_x.load(), p.pan_y.load());
        fprintf(f, "strip.%d.buses", i);
        for (int b = 0; b < kBuses; ++b) fprintf(f, " %d", p.bus_on[b].load());
        fprintf(f, "\n");
        fprintf(f, "strip.%d.duck %d %.3f\n", i, p.duck_key.load(), p.duck_depth_db.load());
        if (i < kHwStrips) {
            fprintf(f, "strip.%d.device %s\n", i, hw[i]);
            if (hwd[i][0]) fprintf(f, "strip.%d.devicedesc %s\n", i, hwd[i]);
        }
    }

    for (int b = 0; b < kBuses; ++b) {
        const BusParams& p = s->bus[b];
        fprintf(f, "bus.%d.gain %.3f\n", b, p.gain_db.load());
        fprintf(f, "bus.%d.mute %d\n", b, p.mute.load());
        fprintf(f, "bus.%d.mono %d\n", b, p.mono.load());
        fprintf(f, "bus.%d.eq %d\n", b, p.eq_on.load());
        fprintf(f, "bus.%d.preamp %.3f\n", b, p.eq_preamp_db.load());
        for (int k = 0; k < kBusEqBands; ++k)
            fprintf(f, "bus.%d.band.%d %.3f %.3f %.3f %d %d\n", b, k,
                    p.eq_gain[k].load(), p.eq_freq[k].load(), p.eq_q[k].load(),
                    p.eq_type[k].load(), p.eq_band_on[k].load());
        if (b < kPhysBuses) {
            fprintf(f, "bus.%d.device %s\n", b, out[b]);
            if (outd[b][0]) fprintf(f, "bus.%d.devicedesc %s\n", b, outd[b]);
        }
    }

    fprintf(f, "duck %d %.3f %.3f %.3f\n", s->duck_enabled.load(),
            s->duck_threshold_db.load(), s->duck_attack_ms.load(), s->duck_release_ms.load());
    fprintf(f, "rec.source_bus %d\n", s->rec.source_bus.load());
    fprintf(f, "rec.gain %.3f\n", s->rec.gain_db.load());
    fprintf(f, "rec.loop %d\n", s->rec.loop.load());
    fprintf(f, "rec.buses");
    for (int b = 0; b < kBuses; ++b) fprintf(f, " %d", s->rec.bus_on[b].load());
    fprintf(f, "\n");

    char lstrip[kStrips][kLabelLen], lbus[kBuses][kLabelLen];
    bool lok = false;
    for (int t = 0; t < 16 && !lok; ++t) lok = labels_read(s->labels, lstrip, lbus);
    if (lok) {
        for (int i = 0; i < kStrips; ++i)
            if (lstrip[i][0]) fprintf(f, "label.strip.%d %s\n", i, lstrip[i]);
        for (int b = 0; b < kBuses; ++b)
            if (lbus[b][0]) fprintf(f, "label.bus.%d %s\n", b, lbus[b]);
    }

    for (int i = 0; i < kVbanStreams; ++i) {
        const VbanOutCfg& o = s->vban.out[i];
        fprintf(f, "vban.out.%d %d %d %d %d %s\n", i, o.enabled, o.source_bus,
                o.port, o.rate, o.name);
        fprintf(f, "vban.out.%d.host %s\n", i, o.host);
        const VbanInCfg& n = s->vban.in[i];
        fprintf(f, "vban.in.%d %d %d %d %s\n", i, n.enabled, n.port, n.rate, n.name);
    }

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Applies a preset onto the live shared state. Bumps the seqlocks so the engine
// picks up device assignment and VBAN changes on its next control poll.
inline bool load_preset(Shared* s, const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    int ver = 0;
    if (sscanf(line, "betterbanana-preset %d", &ver) != 1 || ver > kPresetVersion) {
        fclose(f);
        return false;
    }

    char hw[kHwStrips][kNameLen] = {}, out[kPhysBuses][kNameLen] = {};
    char hwd[kHwStrips][kNameLen] = {}, outd[kPhysBuses][kNameLen] = {};
    bool touched_routing = false;

    // A version 1 preset only describes six bands and carries no preamp, and the
    // band count may grow again later. Track what the file actually mentioned,
    // so anything it does not is reset afterwards rather than left holding the
    // tail of whatever curve was loaded before.
    bool band_seen[kBuses][kBusEqBands] = {};
    bool bus_had_eq[kBuses] = {};
    bool preamp_seen[kBuses] = {};

    // VBAN entries and labels are written as they are parsed, so keep those
    // seqlocks held open for the whole pass; a reader simply retries.
    s->vban.seq.fetch_add(1, std::memory_order_acq_rel);
    s->labels.seq.fetch_add(1, std::memory_order_acq_rel);
    std::memset(s->labels.strip, 0, sizeof(s->labels.strip));
    std::memset(s->labels.bus,   0, sizeof(s->labels.bus));

    while (fgets(line, sizeof(line), f)) {
        // Strip the newline; the value is everything after the first space.
        size_t len = std::strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
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
        else if (keyed("strip.", ".mono", i) && i < kStrips) s->strip[i].mono.store(atoi(val));
        else if (keyed("strip.", ".gate", i) && i < kStrips) s->strip[i].gate.store(atof(val));
        else if (keyed("strip.", ".comp", i) && i < kStrips) s->strip[i].comp.store(atof(val));
        else if (keyed("strip.", ".aud",  i) && i < kStrips) s->strip[i].audibility.store(atof(val));
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
        else if (keyed2("bus.", ".band.", "", i, k) && i < kBuses && k < kBusEqBands) {
            int type = kEqPeak, on = 1;
            const int got = sscanf(val, "%f %f %f %d %d", &a, &b, &c, &type, &on);
            if (got >= 3) {
                s->bus[i].eq_gain[k].store(a);
                s->bus[i].eq_freq[k].store(b);
                s->bus[i].eq_q[k].store(c);
                s->bus[i].eq_type[k].store(got >= 4 && type >= 0 && type < kEqTypeCount
                                           ? type : kEqPeak);
                s->bus[i].eq_band_on[k].store(got >= 5 ? (on ? 1 : 0) : 1);
                band_seen[i][k] = true;
                bus_had_eq[i] = true;
            }
        }
        else if (keyed("bus.", ".preamp", i) && i < kBuses) {
            s->bus[i].eq_preamp_db.store(atof(val));
            preamp_seen[i] = true;
            bus_had_eq[i] = true;
        }
        else if (keyed("bus.", ".gain", i) && i < kBuses) s->bus[i].gain_db.store(atof(val));
        else if (keyed("bus.", ".mute", i) && i < kBuses) s->bus[i].mute.store(atoi(val));
        else if (keyed("bus.", ".mono", i) && i < kBuses) s->bus[i].mono.store(atoi(val));
        else if (keyed("bus.", ".eq",   i) && i < kBuses) s->bus[i].eq_on.store(atoi(val));
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
    fclose(f);

    static const float kSpread[kBusEqBands] = {
        31, 62, 125, 250, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000
    };
    for (int b = 0; b < kBuses; ++b) {
        if (!bus_had_eq[b]) continue;
        for (int k = 0; k < kBusEqBands; ++k) {
            if (band_seen[b][k]) continue;
            s->bus[b].eq_gain[k].store(0.0f);
            s->bus[b].eq_freq[k].store(kSpread[k]);
            s->bus[b].eq_q[k].store(1.0f);
            s->bus[b].eq_type[k].store(kEqPeak);
            s->bus[b].eq_band_on[k].store(1);
        }
        if (!preamp_seen[b]) s->bus[b].eq_preamp_db.store(0.0f);
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

} // namespace bb
