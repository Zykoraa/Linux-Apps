// betterbanana - setting up the stream bus, in one step.
//
// What a Discord screen share needs from the mixer: one A bus pointed at the
// stream sink, the application audio routed to it, the callers (AUX) and your
// microphone kept off it, and the bus pre-fader so your own listening level
// never decides what the viewers hear. Used by `bb-ctl stream setup` and the
// GUI's Stream dialog, so both make exactly the same choices; planned first and
// applied second, so a dry run can say what would change.
//
// The rules, all carried over from the old bb-stream-setup script:
//   - reuse the bus already pointing at the sink; otherwise take the one asked
//     for if it is free, otherwise the highest free bus (A1 is conventionally
//     the one you listen on), and never take one that already has a device;
//   - label it STREAM only if the user has not named it;
//   - route the VAIO strip and every strip fed by a virtual cable - application
//     audio. Not physical inputs: that is your microphone, which Discord already
//     sends as your voice, so viewers would hear you twice;
//   - never AUX, and if AUX is already there, take it off: it is the callers'
//     voices, and on the stream every one of them hears themselves.
#pragma once

#include "protocol.h"
#include "preset.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bb {

struct StreamSetupOpts {
    int  bus = -1;            // A bus to claim, -1 = pick
    bool bus_only = false;    // leave strip routing alone
    bool autolevel = false;   // also switch the bus's auto-level on
};

struct StreamSetupAction {
    enum Kind { ClaimBus, LabelBus, RouteStrip, UnrouteAux, Prefader, AutoLevelOn } kind;
    int strip = -1;
    std::string what;         // human-readable, for a dry run or the dialog
};

struct StreamSetupPlan {
    bool ok = false;
    std::string error;        // why not, when !ok
    int bus = -1;
    bool reused = false;      // the bus already pointed at the sink
    std::vector<StreamSetupAction> actions;
};

// Turns a bus pre-fader (or back). Going pre-fader, each strip's send starts at
// its current fader level, so the bus sounds the same the moment it switches
// and only stops following the faders from then on.
inline void set_prefader(Shared* s, int b, bool on, bool keep_sends = false)
{
    BusParams& p = s->bus[b];
    if (on && !p.prefader.load() && !keep_sends)
        for (int i = 0; i < kStrips; ++i)
            s->strip[i].send_db[b].store(clamp_send(s->strip[i].gain_db.load()));
    p.prefader.store(on ? 1 : 0);
}

inline const char* stream_strip_name(int i)
{
    static const char* const n[kStrips] = { "HW IN 1", "HW IN 2", "HW IN 3", "VAIO", "AUX" };
    return i >= 0 && i < kStrips ? n[i] : "?";
}

inline StreamSetupPlan plan_stream_setup(const Shared* s, const StreamSetupOpts& o)
{
    StreamSetupPlan plan;
    char hw[kHwStrips][kNameLen], bo[kPhysBuses][kNameLen];
    uint32_t seq = 0;
    bool got = false;
    for (int t = 0; t < 16 && !got; ++t)
        got = routing_read(s->routing, seq, hw, bo);
    if (!got) { plan.error = "could not read the routing (the engine is busy) - try again"; return plan; }
    char ls[kStrips][kLabelLen], lb[kBuses][kLabelLen];
    bool lok = false;
    for (int t = 0; t < 16 && !lok; ++t)
        lok = labels_read(s->labels, ls, lb);

    char name[16];
    // ---- the bus -----------------------------------------------------------
    for (int b = 0; b < kPhysBuses && plan.bus < 0; ++b)
        if (std::strcmp(bo[b], kStreamSinkName) == 0) { plan.bus = b; plan.reused = true; }
    if (plan.bus < 0) {
        if (o.bus >= 0) {
            if (o.bus >= kPhysBuses) { plan.error = "the stream bus must be A1, A2 or A3"; return plan; }
            if (bo[o.bus][0]) {
                plan.error = std::string("A") + std::to_string(o.bus + 1) + " already plays to \""
                           + bo[o.bus] + "\" - free it first, or pick another bus";
                return plan;
            }
            plan.bus = o.bus;
        } else {
            for (int b = kPhysBuses - 1; b >= 0 && plan.bus < 0; --b)
                if (!bo[b][0]) plan.bus = b;
            if (plan.bus < 0) {
                plan.error = "every output bus already has a device - free one, or say which to use";
                return plan;
            }
        }
        std::snprintf(name, sizeof(name), "A%d", plan.bus + 1);
        plan.actions.push_back({ StreamSetupAction::ClaimBus, -1,
                                 std::string("point ") + name + " at the stream sink" });
        if (!lok || !lb[plan.bus][0])
            plan.actions.push_back({ StreamSetupAction::LabelBus, -1,
                                     std::string("name ") + name + " \"STREAM\"" });
    }
    const int b = plan.bus;
    std::snprintf(name, sizeof(name), "A%d", b + 1);

    // ---- the strips ----------------------------------------------------------
    if (!o.bus_only) {
        for (int i = 0; i < kStrips - 1; ++i) {
            const bool vaio  = i == kHwStrips;
            const bool cable = i < kHwStrips &&
                               std::strncmp(hw[i], kCablePrefix, std::strlen(kCablePrefix)) == 0;
            if (!vaio && !cable) continue;
            if (s->strip[i].bus_on[b].load()) continue;
            plan.actions.push_back({ StreamSetupAction::RouteStrip, i,
                                     std::string("route ") + stream_strip_name(i) + " to " + name });
        }
    }
    if (s->strip[kStrips - 1].bus_on[b].load())
        plan.actions.push_back({ StreamSetupAction::UnrouteAux, kStrips - 1,
                                 std::string("take AUX off ") + name
                                 + " (callers would hear themselves)" });

    if (!s->bus[b].prefader.load())
        plan.actions.push_back({ StreamSetupAction::Prefader, -1,
                                 std::string("make ") + name + " pre-fader (sends start at the faders)" });
    if (o.autolevel && !s->bus[b].al_on.load())
        plan.actions.push_back({ StreamSetupAction::AutoLevelOn, -1,
                                 std::string("switch on auto-level for ") + name });
    plan.ok = true;
    return plan;
}

inline void apply_stream_setup(Shared* s, const StreamSetupPlan& plan)
{
    if (!plan.ok || plan.bus < 0) return;
    const int b = plan.bus;
    for (const auto& a : plan.actions) {
        switch (a.kind) {
        case StreamSetupAction::ClaimBus:
            routing_write_begin(s->routing);
            std::snprintf(s->routing.bus_out[b], kNameLen, "%s", kStreamSinkName);
            s->routing.bus_out_desc[b][0] = 0;
            ++s->routing.bus_out_gen[b];
            routing_write_end(s->routing);
            break;
        case StreamSetupAction::LabelBus:
            s->labels.seq.fetch_add(1, std::memory_order_acq_rel);
            std::snprintf(s->labels.bus[b], kLabelLen, "%s", "STREAM");
            s->labels.seq.fetch_add(1, std::memory_order_release);
            break;
        case StreamSetupAction::RouteStrip:
            s->strip[a.strip].bus_on[b].store(1);
            break;
        case StreamSetupAction::UnrouteAux:
            s->strip[a.strip].bus_on[b].store(0);
            break;
        case StreamSetupAction::Prefader:
            set_prefader(s, b, true);
            break;
        case StreamSetupAction::AutoLevelOn:
            s->bus[b].al_on.store(1);
            break;
        }
    }
}

} // namespace bb
