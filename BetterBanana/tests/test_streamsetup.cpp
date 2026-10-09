// Stream setup: the choices `bb-ctl stream setup` and the Stream dialog make.
//
// Most of these are about what it must NOT do - take a bus that is in use,
// route the callers or the microphone to the stream, overwrite a name someone
// chose - because those are the mistakes it exists to prevent.
#include "../common/streamsetup.h"

#include <cstdio>
#include <memory>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static std::unique_ptr<Shared> fresh()
{
    auto s = std::make_unique<Shared>();
    set_defaults(s.get());
    return s;
}

static void route_out(Shared* s, int b, const char* name)
{
    routing_write_begin(s->routing);
    std::snprintf(s->routing.bus_out[b], kNameLen, "%s", name);
    routing_write_end(s->routing);
}

static void route_in(Shared* s, int i, const char* name)
{
    routing_write_begin(s->routing);
    std::snprintf(s->routing.hw_in[i], kNameLen, "%s", name);
    routing_write_end(s->routing);
}

static bool has(const StreamSetupPlan& p, StreamSetupAction::Kind k, int strip = -1)
{
    for (const auto& a : p.actions) if (a.kind == k && (strip < 0 || a.strip == strip)) return true;
    return false;
}

int main()
{
    // --- a typical mixer: mic on HW IN 1, a cable on HW IN 2 ---------------
    {
        auto s = fresh();
        route_out(s.get(), 0, "alsa_output.headphones");
        route_in(s.get(), 0, "alsa_input.mic");
        route_in(s.get(), 1, "cable:0");
        s->strip[1].gain_db.store(-20.0f);
        const StreamSetupPlan p = plan_stream_setup(s.get(), {});
        chk(p.ok && p.bus == 2 && !p.reused, "the highest free bus is claimed");
        chk(has(p, StreamSetupAction::ClaimBus) && has(p, StreamSetupAction::LabelBus),
            "pointed at the sink and named");
        chk(has(p, StreamSetupAction::RouteStrip, 1), "the cable strip is routed");
        chk(has(p, StreamSetupAction::RouteStrip, 3), "and VAIO");
        chk(!has(p, StreamSetupAction::RouteStrip, 0), "but never the microphone");
        chk(!has(p, StreamSetupAction::RouteStrip, 2), "nor an unassigned strip");
        chk(!has(p, StreamSetupAction::RouteStrip, 4), "nor AUX");
        chk(has(p, StreamSetupAction::RouteStrip, kVaio3Strip), "VAIO3 is an app strip too");
        chk(has(p, StreamSetupAction::Prefader), "and the bus goes pre-fader");

        apply_stream_setup(s.get(), p);
        char hw[kHwStrips][kNameLen], bo[kPhysBuses][kNameLen];
        uint32_t seq = 0;
        routing_read(s->routing, seq, hw, bo);
        chk(std::strcmp(bo[2], kStreamSinkName) == 0, "A3 now plays to the stream sink");
        chk(std::strcmp(s->labels.bus[2], "STREAM") == 0, "and is called STREAM");
        chk(s->strip[1].bus_on[2].load() && s->strip[3].bus_on[2].load(), "with the apps on it");
        chk(!s->strip[0].bus_on[2].load() && !s->strip[4].bus_on[2].load(), "and not the mic or AUX");
        chk(s->bus[2].prefader.load() == 1, "pre-fader");
        chk(s->strip[1].send_db[2].load() == -20.0f,
            "with each send starting at its fader, so nothing jumps");

        const StreamSetupPlan again = plan_stream_setup(s.get(), {});
        chk(again.ok && again.reused && again.bus == 2, "running it again finds the same bus");
        chk(again.actions.empty(), "and has nothing left to do");
    }

    // --- callers already on the stream are taken off -----------------------
    {
        auto s = fresh();
        route_out(s.get(), 2, kStreamSinkName);
        s->strip[4].bus_on[2].store(1);
        const StreamSetupPlan p = plan_stream_setup(s.get(), {});
        chk(p.reused && has(p, StreamSetupAction::UnrouteAux), "AUX found on the stream bus is removed");
        apply_stream_setup(s.get(), p);
        chk(!s->strip[4].bus_on[2].load(), "and is gone");
    }

    // --- a name someone chose stays ----------------------------------------
    {
        auto s = fresh();
        std::snprintf(s->labels.bus[2], kLabelLen, "%s", "viewers");
        const StreamSetupPlan p = plan_stream_setup(s.get(), {});
        chk(p.ok && !has(p, StreamSetupAction::LabelBus), "a bus with its own name is not renamed");
    }

    // --- a bus in use is never taken ---------------------------------------
    {
        auto s = fresh();
        route_out(s.get(), 2, "alsa_output.speakers");
        const StreamSetupPlan p = plan_stream_setup(s.get(), {});
        chk(p.ok && p.bus == 1, "a used A3 is passed over for A2");

        StreamSetupOpts want3;
        want3.bus = 2;
        const StreamSetupPlan q = plan_stream_setup(s.get(), want3);
        chk(!q.ok && !q.error.empty(), "asking for a bus in use is refused, with a reason");

        route_out(s.get(), 0, "a"); route_out(s.get(), 1, "b");
        const StreamSetupPlan r = plan_stream_setup(s.get(), {});
        chk(!r.ok, "with every bus in use it refuses rather than guess");
    }

    // --- options ------------------------------------------------------------
    {
        auto s = fresh();
        route_in(s.get(), 1, "cable:0");
        StreamSetupOpts o;
        o.bus_only = true;
        o.autolevel = true;
        const StreamSetupPlan p = plan_stream_setup(s.get(), o);
        chk(!has(p, StreamSetupAction::RouteStrip), "--bus-only leaves the strips alone");
        chk(has(p, StreamSetupAction::AutoLevelOn), "--autolevel switches auto-level on");
    }

    // --- pre-fader switching ------------------------------------------------
    {
        auto s = fresh();
        s->strip[0].gain_db.store(-12.0f);
        s->strip[0].send_db[2].store(3.0f);
        set_prefader(s.get(), 2, true, true);
        chk(s->strip[0].send_db[2].load() == 3.0f, "keep-sends leaves the sends as they were");
        set_prefader(s.get(), 2, false);
        set_prefader(s.get(), 2, true);
        chk(s->strip[0].send_db[2].load() == -12.0f, "otherwise they start at the faders");
        s->strip[0].send_db[2].store(5.0f);
        set_prefader(s.get(), 2, true);
        chk(s->strip[0].send_db[2].load() == 5.0f, "and a bus already pre-fader keeps its sends");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
