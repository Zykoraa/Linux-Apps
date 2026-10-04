// The stream guard's decisions, against graphs shaped like the real one.
//
// The graph below is modelled on a live Discord entire-screen share: one
// discord_capture per playback stream, each naming its stream by
// object.serial - including one for BetterBanana A3 itself. The ids and
// serials are deliberately different (bb_a3 is node 60, serial 61), because
// matching the capture on the id instead of the serial is the easy mistake.
#include "../engine/streamguard.h"

#include <algorithm>
#include <cstdio>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

struct Builder {
    Graph g;
    uint32_t next_port = 1000, next_link = 5000;

    // A node with stereo ports. Outputs for a playback stream, inputs (and
    // monitor outputs, which must be ignored) for a capture.
    void node(uint32_t id, uint64_t serial, const char* name, const char* cls,
              bool in_ports, bool out_ports, bool monitors = false)
    {
        g.nodes[id] = { name, "", cls, serial };
        for (const char* ch : { "FL", "FR" }) {
            if (in_ports)  g.ports[next_port++] = { id, true,  false, ch };
            if (out_ports) g.ports[next_port++] = { id, false, false, ch };
            if (monitors)  g.ports[next_port++] = { id, false, true,  ch };
        }
    }
    uint32_t port(uint32_t node, bool input, const char* ch) const
    {
        for (const auto& [id, p] : g.ports)
            if (p.node == node && p.input == input && !p.monitor && p.channel == ch) return id;
        return 0;
    }
    void link(uint32_t out_node, uint32_t in_node)
    {
        for (const char* ch : { "FL", "FR" })
            g.links[next_link++] = { port(out_node, false, ch), port(in_node, true, ch) };
    }
    void capture(uint32_t id, const char* target)
    {
        node(id, id + 1, kCaptureName, "Stream/Input/Audio", true, false, true);
        g.capture_target[id] = target;
    }
};

// The usual scene: three buses, two apps, Discord capturing all of them.
static Builder scene()
{
    Builder b;
    b.node(58, 59,   "bb_a1",     "Stream/Output/Audio", false, true);
    b.node(59, 7722, "bb_a2",     "Stream/Output/Audio", false, true);
    b.node(60, 61,   "bb_a3",     "Stream/Output/Audio", false, true);
    b.node(276, 302, "Chromium",  "Stream/Output/Audio", false, true);
    b.node(238, 7076, "Brave",    "Stream/Output/Audio", false, true);
    b.capture(311, "302");    // Discord's own sounds
    b.capture(315, "61");     // BetterBanana A3: the one to feed
    b.capture(316, "59");     // BetterBanana A1
    b.capture(314, "7722");   // BetterBanana A2
    b.capture(367, "7076");   // Brave
    return b;
}

static bool dropped(const GuardPlan& p, uint32_t lid)
{
    return std::any_of(p.drop.begin(), p.drop.end(), [&](auto& d) { return d.first == lid; });
}

// Applies a plan to the graph, as the engine would.
static void apply(Graph& g, const GuardPlan& p, uint32_t& next_link)
{
    for (auto& [lid, why] : p.drop) g.links.erase(lid);
    for (auto& [o, i] : p.make) g.links[next_link++] = { o, i };
}

static int links_into(const Graph& g, uint32_t node)
{
    int n = 0;
    for (const auto& [lid, l] : g.links) if (g.ports.at(l.in_port).node == node) ++n;
    return n;
}

int main()
{
    GuardInput on;
    on.mode = kGuardModeOn;
    on.stream_bus = 2;

    // --- the stream bus goes into Discord's own capture of it, once --------
    {
        Builder b = scene();
        b.link(276, 311);   // Discord linked its own sounds
        b.link(238, 367);   // and Brave, directly
        b.link(58, 316);    // and A1 - which carries the callers
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.state == kGuardOwnCapture, "the stream bus is fed into the capture made for it");
        chk(p.captures == 5, "every capture is counted");
        chk(p.make.size() == 2, "as one stereo pair");
        bool right = true;
        for (auto& [o, i] : p.make) {
            right = right && b.g.ports.at(o).node == 60 && b.g.ports.at(i).node == 315
                          && b.g.ports.at(o).channel == b.g.ports.at(i).channel;
        }
        chk(right, "bb_a3 into capture 315, left to left and right to right");
        chk(p.drop.size() == 6, "and every other link into Discord is removed");
        int echo = 0, dup = 0;
        for (auto& [lid, why] : p.drop) (why == kDropEcho ? echo : dup)++;
        chk(echo == 2, "A1 counts as echo");
        chk(dup == 4, "the apps count as duplicates");

        apply(b.g, p, b.next_link);
        const GuardPlan again = plan_guard(b.g, on);
        chk(again.drop.empty() && again.make.empty(), "and applying it leaves nothing more to do");
        chk(links_into(b.g, 315) == 2 && links_into(b.g, 311) == 0 && links_into(b.g, 367) == 0,
            "one copy of the bus, nothing else, in the whole capture set");
    }

    // --- the serial, not the id --------------------------------------------
    {
        Builder b = scene();
        b.g.capture_target[315] = "60";   // the id, not the serial: not ours
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.state == kGuardFallback, "a capture naming bb_a3's id rather than serial is not its own");
    }

    // --- a second copy of the bus is a duplicate ---------------------------
    // This is the chamber: the bus fed into several captures, each a few
    // milliseconds apart.
    {
        Builder b = scene();
        b.link(60, 315);
        b.link(60, 311);
        b.link(60, 316);
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.make.empty(), "the copy that belongs stays");
        chk(p.drop.size() == 4, "the two extra copies go");
        for (auto& [lid, l] : b.g.links)
            if (b.g.ports.at(l.in_port).node == 315) chk(!dropped(p, lid), "never the one that belongs");
    }

    // --- window share: no capture of the bus, so feed them all -------------
    {
        Builder b;
        b.node(60, 61, "bb_a3", "Stream/Output/Audio", false, true);
        b.node(238, 7076, "Brave", "Stream/Output/Audio", false, true);
        b.capture(367, "7076");
        b.link(238, 367);
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.state == kGuardFallback, "sharing one window falls back and says so");
        chk(p.make.size() == 2 && dropped(p, 5000), "the bus replaces the app in its capture");
    }

    // --- monitor ports are not inputs --------------------------------------
    {
        Builder b = scene();
        const GuardPlan p = plan_guard(b.g, on);
        for (auto& [o, i] : p.make) chk(!b.g.ports.at(i).monitor && b.g.ports.at(i).input,
                                        "nothing is linked into a capture's monitor");
    }

    // --- nothing capturing, nothing to do ----------------------------------
    {
        Builder b;
        b.node(60, 61, "bb_a3", "Stream/Output/Audio", false, true);
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.state == kGuardIdle && p.drop.empty() && p.make.empty(), "no Discord, no plan");
    }

    // --- the stream bus node is missing ------------------------------------
    // Assigned but not up (the engine is reconnecting it). Do not strip the
    // apps out of Discord when there is nothing to replace them with - but
    // still keep the callers out.
    {
        Builder b = scene();
        b.g.nodes.erase(60);
        for (auto it = b.g.ports.begin(); it != b.g.ports.end();)
            it = it->second.node == 60 ? b.g.ports.erase(it) : std::next(it);
        b.link(238, 367);
        b.link(58, 316);
        const GuardPlan p = plan_guard(b.g, on);
        chk(p.state == kGuardBusMissing, "a missing bus node is reported");
        chk(p.drop.size() == 2, "only the bus carrying the callers is removed");
        chk(p.make.empty(), "and nothing is linked");
    }

    // --- echo-only mode, and no stream bus ---------------------------------
    {
        Builder b = scene();
        b.link(58, 316);      // A1 carries AUX
        b.link(59, 314);      // A2 does not
        b.link(238, 367);
        GuardInput echo;
        echo.mode = kGuardModeEcho;
        echo.stream_bus = 2;
        echo.aux_on[0] = true;
        GuardPlan p = plan_guard(b.g, echo);
        chk(p.state == kGuardEchoOnly, "echo-only mode says so");
        chk(p.drop.size() == 2 && p.make.empty(), "and removes only the bus carrying AUX");

        GuardInput none;
        none.stream_bus = -1;
        none.aux_on[0] = true;
        p = plan_guard(b.g, none);
        chk(p.state == kGuardEchoOnly && p.drop.size() == 2 && p.make.empty(),
            "without a stream bus the guard still keeps the callers out, and nothing more");
    }

    // --- off is off ---------------------------------------------------------
    {
        Builder b = scene();
        b.link(58, 316);
        GuardInput off;
        off.mode = kGuardModeOff;
        off.stream_bus = 2;
        off.aux_on[0] = true;
        const GuardPlan p = plan_guard(b.g, off);
        chk(p.state == kGuardOff && p.drop.empty() && p.make.empty(), "guard off touches nothing");
    }

    // --- port names ---------------------------------------------------------
    chk(port_channel("FL", "input_FR") == "FL", "audio.channel wins over the port name");
    chk(port_channel(nullptr, "monitor_FR") == "FR", "else the end of the port name");
    chk(port_channel("", "capture_MONO") == "MONO", "including a mono port");

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
