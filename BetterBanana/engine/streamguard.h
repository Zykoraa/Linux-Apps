// betterbanana - the stream guard: what may feed Discord's screen-share capture.
//
// Discord, sharing the entire screen, captures every playback stream itself:
// one node called "discord_capture" per stream, each with target.object naming
// (by object.serial) the stream it records, and it MIXES them all into what the
// viewers hear. Left alone, that goes wrong three ways:
//
//   echo      it captures BetterBanana's own A buses too, and those carry the
//             AUX strip - the callers' voices - so everyone in the call hears
//             themselves inside the stream;
//   doubling  an app that also reaches the stream bus is heard twice, a few
//             milliseconds apart, and its direct copy drops out whenever the
//             app pauses or reopens its stream;
//   chamber   the stream bus fed into several captures is heard several
//             times, each copy offset by its own buffering: comb filtering,
//             which sounds hollow, like a tunnel or a chamber.
//
// So the stream bus goes into exactly ONE capture - the one Discord made for
// the bus's own stream - and nothing else goes into any of them. If Discord is
// not capturing the bus at all (a single window is shared, not the screen),
// the bus is fed into every capture instead, and the state says so.
//
// This is the decision only, from a snapshot of the graph: no PipeWire, so
// every rule is tested in tests/test_streamguard.cpp against graphs shaped like
// real ones. The engine applies the plan (engine.cpp, apply_guard).
#pragma once

#include "nodes.h"
#include "../common/protocol.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bb {

constexpr const char* kCaptureName = "discord_capture";

struct PortInfo {
    uint32_t    node = 0;
    bool        input = false;
    bool        monitor = false;   // a capture node's own monitor outputs
    std::string channel;           // "FL", "FR", ...
};

struct LinkInfo { uint32_t out_port = 0, in_port = 0; };

struct Graph {
    NodeMap                          nodes;
    std::map<uint32_t, PortInfo>     ports;
    std::map<uint32_t, LinkInfo>     links;
    // discord_capture node id -> its target.object. Not in the registry's
    // global properties; the engine binds each capture node to read it.
    std::map<uint32_t, std::string>  capture_target;
};

struct GuardInput {
    int  mode = kGuardModeOn;          // StreamGuardMode
    int  stream_bus = -1;              // A bus index, -1 = none
    bool aux_on[kPhysBuses] = {};      // AUX strip routed to A1..A3
};

enum DropWhy { kDropEcho, kDropDuplicate };

struct GuardPlan {
    std::vector<std::pair<uint32_t, DropWhy>>  drop;   // link ids
    std::vector<std::pair<uint32_t, uint32_t>> make;   // (out port, in port)
    int state = kGuardIdle;                            // StreamGuardState
    int captures = 0;
};

// "FL" from audio.channel, or from the end of port.name ("output_FL").
inline std::string port_channel(const char* audio_channel, const char* port_name)
{
    if (audio_channel && *audio_channel) return audio_channel;
    if (!port_name) return {};
    const std::string n = port_name;
    const size_t u = n.rfind('_');
    return u == std::string::npos ? n : n.substr(u + 1);
}

inline std::string bus_node_name(int b) { return "bb_a" + std::to_string(b + 1); }

inline GuardPlan plan_guard(const Graph& g, const GuardInput& in)
{
    GuardPlan plan;

    std::set<uint32_t> cap_nodes;
    for (const auto& [id, n] : g.nodes)
        if (n.name == kCaptureName && n.media_class.rfind("Stream/Input/Audio", 0) == 0)
            cap_nodes.insert(id);
    plan.captures = (int)cap_nodes.size();

    std::set<uint32_t> cap_in;                       // every capture input port
    for (const auto& [id, p] : g.ports)
        if (p.input && !p.monitor && cap_nodes.count(p.node)) cap_in.insert(id);

    if (in.mode == kGuardModeOff) { plan.state = kGuardOff; return plan; }
    if (cap_in.empty())           { plan.state = kGuardIdle; return plan; }

    // Each A bus's node and output ports.
    uint32_t bus_node[kPhysBuses];
    for (int b = 0; b < kPhysBuses; ++b) bus_node[b] = 0;
    for (const auto& [id, n] : g.nodes)
        for (int b = 0; b < kPhysBuses; ++b)
            if (n.name == bus_node_name(b)) bus_node[b] = id;
    std::set<uint32_t> bus_out[kPhysBuses];
    for (const auto& [id, p] : g.ports)
        for (int b = 0; b < kPhysBuses; ++b)
            if (bus_node[b] && p.node == bus_node[b] && !p.input && !p.monitor)
                bus_out[b].insert(id);

    const int sb = (in.stream_bus >= 0 && in.stream_bus < kPhysBuses) ? in.stream_bus : -1;
    const bool exclusive = in.mode == kGuardModeOn && sb >= 0;

    // With a stream bus every other bus is kept out: it either carries AUX
    // (echo) or duplicates what the stream bus already delivers. Without one -
    // or in echo-only mode - only the buses carrying AUX are.
    std::set<uint32_t> blocked;
    for (int b = 0; b < kPhysBuses; ++b) {
        if (b == sb) continue;
        if (exclusive || in.aux_on[b]) blocked.insert(bus_out[b].begin(), bus_out[b].end());
    }

    std::set<uint32_t> stream_out;
    if (exclusive) stream_out = bus_out[sb];

    // The capture Discord made for the stream bus's own playback stream.
    std::set<uint32_t> feed;
    if (exclusive && bus_node[sb]) {
        const auto it = g.nodes.find(bus_node[sb]);
        const std::string serial = it != g.nodes.end() && it->second.serial
                                 ? std::to_string(it->second.serial) : std::string();
        std::set<uint32_t> own_nodes;
        if (!serial.empty())
            for (const auto& [id, t] : g.capture_target)
                if (cap_nodes.count(id) && t == serial) own_nodes.insert(id);
        for (uint32_t p : cap_in)
            if (own_nodes.count(g.ports.at(p).node)) feed.insert(p);
    }

    if (!exclusive)               plan.state = kGuardEchoOnly;
    else if (stream_out.empty())  plan.state = kGuardBusMissing;
    else if (feed.empty())      { plan.state = kGuardFallback; feed = cap_in; }
    else                          plan.state = kGuardOwnCapture;

    std::set<std::pair<uint32_t, uint32_t>> have;
    for (const auto& [lid, l] : g.links) {
        if (!cap_in.count(l.in_port)) continue;
        have.insert({ l.out_port, l.in_port });
        if (blocked.count(l.out_port)) {
            plan.drop.push_back({ lid, kDropEcho });
        } else if (exclusive && !stream_out.empty() && !stream_out.count(l.out_port)) {
            plan.drop.push_back({ lid, kDropDuplicate });          // an app, captured directly
        } else if (stream_out.count(l.out_port) && !feed.count(l.in_port)) {
            plan.drop.push_back({ lid, kDropDuplicate });          // a second copy of the bus
        }
    }

    // FL to FL, FR to FR, into the capture(s) chosen above.
    for (uint32_t src : stream_out) {
        const std::string& ch = g.ports.at(src).channel;
        for (uint32_t dst : feed)
            if (g.ports.at(dst).channel == ch && !have.count({ src, dst }))
                plan.make.push_back({ src, dst });
    }
    return plan;
}

} // namespace bb
