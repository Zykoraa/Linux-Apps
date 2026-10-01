// betterbanana - finding a saved device again.
//
// A preset stores each endpoint's node.name and, beside it, the device's
// description. Node names encode the USB port, so they move when hardware is
// replugged; when the saved name is gone the description is how the device is
// found again.
//
// The match has to respect direction. A sound card's input and output often
// share one description - here "Ryzen HD Audio Controller Pro" is both
// pro-input-0 and pro-output-0 - and a match that ignored it once pointed bus
// A2 at the capture side. A strip records from a source and a bus plays to a
// sink; Audio/Duplex is both. Streams (Stream/...) are never devices.
//
// Nodes are kept per id, not per name: names are not unique (Discord runs
// several nodes all called discord_capture), so a map keyed by name forgot a
// name the moment any one of its nodes went away.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace bb {

struct NodeInfo {
    std::string name;
    std::string desc;
    std::string media_class;
};

using NodeMap = std::map<uint32_t, NodeInfo>;

enum class Direction { Capture, Playback };

inline bool node_fits(const std::string& media_class, Direction dir)
{
    if (media_class.rfind("Audio/Duplex", 0) == 0) return true;
    return media_class.rfind(dir == Direction::Capture ? "Audio/Source" : "Audio/Sink", 0) == 0;
}

inline const NodeInfo* find_node(const NodeMap& nodes, const std::string& name)
{
    for (const auto& kv : nodes)
        if (kv.second.name == name) return &kv.second;
    return nullptr;
}

// The node a saved endpoint means now: `name` while any node still carries it
// (an explicit assignment is not second-guessed), else the first device of the
// right direction advertising `desc`, else `name` unchanged so the endpoint
// keeps waiting for its device to come back.
inline std::string resolve_node(const NodeMap& nodes, const std::string& name,
                                const std::string& desc, Direction dir)
{
    if (name.empty() || find_node(nodes, name)) return name;
    if (desc.empty()) return name;
    for (const auto& kv : nodes)
        if (kv.second.desc == desc && node_fits(kv.second.media_class, dir))
            return kv.second.name;
    return name;
}

} // namespace bb
