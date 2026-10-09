// betterbanana engine - the audio core.
//
// Topology (all nodes share node.group="betterbanana" so PipeWire schedules them
// under a single driver, which the probe in tests/ verified):
//
//   strips 0..2  Hardware Input 1..3   <- Stream/Input/Audio, target = a source
//   strips 3..4  VAIO / AUX            <- Audio/Sink   (apps play INTO these)
//   buses  0..2  A1 / A2 / A3          -> Stream/Output/Audio, target = a sink
//   buses  3..4  B1 / B2               -> Audio/Source (apps record FROM these)
//
// Input endpoints push into per-strip rings; output endpoints pull from
// per-bus rings and run the mixer on demand. Every endpoint runs on the same
// data-loop thread, so the mixer needs no locking.
#include "../common/protocol.h"
#include "../common/preset.h"
#include "dsp.h"
#include "spectrum.h"
#include "surround.h"
#include "delay.h"
#include "click.h"
#include "loudness.h"
#include "voicefx.h"
#include "nodes.h"
#include "matrix.h"
#include "autolevel.h"
#include "streamguard.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <sndfile.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <vector>

using namespace bb;

static constexpr uint32_t kRate       = 48000;
static constexpr uint32_t kRingFrames = 32768;
static constexpr uint32_t kMaxChunk   = 2048;
static constexpr uint32_t kResyncQuanta = 4;   // drop backlog past this

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// SPSC ring of interleaved stereo frames.
// ---------------------------------------------------------------------------
struct Ring {
    float buf[kRingFrames * kChan] = {};
    std::atomic<uint32_t> wr{0}, rd{0};

    uint32_t avail() const
    {
        return (wr.load(std::memory_order_acquire) + kRingFrames
                - rd.load(std::memory_order_relaxed)) % kRingFrames;
    }
    void clear() { rd.store(wr.load(std::memory_order_relaxed), std::memory_order_release); }

    void write(const float* src, uint32_t frames)
    {
        uint32_t w = wr.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t o = ((w + i) % kRingFrames) * kChan;
            buf[o] = src[i * kChan]; buf[o + 1] = src[i * kChan + 1];
        }
        wr.store((w + frames) % kRingFrames, std::memory_order_release);
    }
    void write_silence(uint32_t frames)
    {
        uint32_t w = wr.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t o = ((w + i) % kRingFrames) * kChan;
            buf[o] = 0.0f; buf[o + 1] = 0.0f;
        }
        wr.store((w + frames) % kRingFrames, std::memory_order_release);
    }
    // Reads `frames`, zero-padding whatever isn't there yet.
    void read_padded(float* dst, uint32_t frames)
    {
        const uint32_t have = avail();
        const uint32_t n = have < frames ? have : frames;
        uint32_t r = rd.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t o = ((r + i) % kRingFrames) * kChan;
            dst[i * kChan] = buf[o]; dst[i * kChan + 1] = buf[o + 1];
        }
        for (uint32_t i = n; i < frames; ++i) { dst[i * kChan] = 0.0f; dst[i * kChan + 1] = 0.0f; }
        rd.store((r + n) % kRingFrames, std::memory_order_release);
    }
    void drop_to(uint32_t keep)
    {
        const uint32_t have = avail();
        if (have <= keep) return;
        const uint32_t drop = have - keep;
        rd.store((rd.load(std::memory_order_relaxed) + drop) % kRingFrames, std::memory_order_release);
    }
};

// ---------------------------------------------------------------------------
// A twelve-band parametric chain. Strips and buses each own one; the only
// difference is which EqParams block feeds it. Engine-private, like the rest
// of the DSP state - shm carries the settings, not the filter memory.
// ---------------------------------------------------------------------------
struct EqChain {
    Biquad     bq[kChan][kEqBands];
    SmoothGain pre[kChan];          // preamp, so a boosted curve can be pulled back
    float c_g[kEqBands] = {}, c_f[kEqBands] = {}, c_q[kEqBands] = {};
    int   c_t[kEqBands] = {}, c_on[kEqBands] = {};
    // Compact list of the bands actually doing something. A twelve-band EQ with
    // three real bands should cost three biquads per sample, not twelve.
    int   active[kEqBands] = {};
    int   n_active = 0;
    bool  init = false;

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) { pre[c].configure(sr, 15.0f); pre[c].snap(1.0f); }
    }

    void update(const EqParams& p, float sr)
    {
        bool rebuild = !init;
        for (int k = 0; k < kEqBands; ++k) {
            const float g  = p.gain[k].load(std::memory_order_relaxed);
            const float f  = p.freq[k].load(std::memory_order_relaxed);
            const float q  = p.q[k].load(std::memory_order_relaxed);
            const int   t  = p.type[k].load(std::memory_order_relaxed);
            const int   on = p.band_on[k].load(std::memory_order_relaxed) ? 1 : 0;
            if (!init || g != c_g[k] || f != c_f[k] || q != c_q[k]
                      || t != c_t[k] || on != c_on[k]) {
                const bool enabling = init && on && !c_on[k];
                for (int c = 0; c < kChan; ++c) {
                    // A band that sat bypassed still holds stale samples in its
                    // delay line; clear them or switching it back on clicks.
                    if (enabling) bq[c][k].reset();
                    design_band(bq[c][k], t, sr, f, q, g);
                }
                c_g[k] = g; c_f[k] = f; c_q[k] = q; c_t[k] = t; c_on[k] = on;
                rebuild = true;
            }
        }
        if (rebuild) {
            n_active = 0;
            for (int k = 0; k < kEqBands; ++k) {
                if (!c_on[k]) continue;
                // A flat peak or shelf designs to a pure bypass, so it can be
                // dropped from the chain outright.
                const bool shelf_or_peak = c_t[k] == kEqPeak || c_t[k] == kEqLowShelf
                                                            || c_t[k] == kEqHighShelf;
                if (shelf_or_peak && std::fabs(c_g[k]) < 1e-4f) continue;
                active[n_active++] = k;
            }
        }
        const float pa = db_to_lin(p.preamp_db.load(std::memory_order_relaxed));
        for (int c = 0; c < kChan; ++c) pre[c].set_target(pa);
        init = true;
    }

    inline float process(int c, float x)
    {
        x *= pre[c].next();
        for (int k = 0; k < n_active; ++k) x = bq[c][active[k]].process(x);
        return x;
    }
};

// ---------------------------------------------------------------------------
// Per-strip DSP state (not in shm - this is engine-private).
// ---------------------------------------------------------------------------
struct StripDsp {
    Gate       gate[kChan];
    Compressor comp[kChan];
    Biquad     eq_lo[kChan], eq_mid[kChan], eq_hi[kChan], aud[kChan];
    EqChain    par;                 // the twelve-band block, after the tone knobs
    VoiceFxChain fx;                // the voice changer, after the EQ
    SmoothGain gain[kChan];
    PeakMeter  pre[kChan], post[kChan];
    Delay      delay;
    // The pre-fader tap gets its own copy of the strip delay, so lip-sync
    // carries to a pre-fader bus too. Only run while some bus wants the tap.
    Delay      send_delay;
    bool       send_live = false;
    float c_gate = -1, c_comp = -1, c_aud = -1;
    float c_lo = 1e9f, c_mid = 1e9f, c_hi = 1e9f;

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) {
            gate[c].configure(sr); comp[c].configure(sr);
            gain[c].configure(sr, 15.0f); gain[c].snap(1.0f);
            pre[c].configure(sr); post[c].configure(sr);
        }
        par.configure(sr);
        fx.configure(sr);
        delay.configure(sr);
        send_delay.configure(sr);
    }
    void update(const StripParams& p, float sr)
    {
        const float g = p.gate.load(std::memory_order_relaxed);
        const float k = p.comp.load(std::memory_order_relaxed);
        const float a = p.audibility.load(std::memory_order_relaxed);
        const float lo = p.eq_low.load(std::memory_order_relaxed);
        const float md = p.eq_mid.load(std::memory_order_relaxed);
        const float hi = p.eq_high.load(std::memory_order_relaxed);
        for (int c = 0; c < kChan; ++c) {
            if (g != c_gate) gate[c].set_knob(g);
            if (k != c_comp) comp[c].set_knob(k);
            if (lo != c_lo)  eq_lo[c].set_lowshelf (sr, 100.0f,  0.707f, lo);
            if (md != c_mid) eq_mid[c].set_peaking (sr, 1000.0f, 0.700f, md);
            if (hi != c_hi)  eq_hi[c].set_highshelf(sr, 8000.0f, 0.707f, hi);
            // "Audibility" lifts presence and thins the low end together.
            if (a != c_aud)  aud[c].set_peaking(sr, 2500.0f, 0.9f, a * 1.2f);
        }
        c_gate = g; c_comp = k; c_aud = a; c_lo = lo; c_mid = md; c_hi = hi;
        par.update(p.eq, sr);
        fx.update(p.fx, sr);
    }
};

struct BusDsp {
    EqChain    eq;
    AutoLevel  al;                  // after the EQ, before the fader
    SmoothGain gain[kChan];
    PeakMeter  meter[kChan];
    Delay      delay;
    Loudness   loud;

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) {
            gain[c].configure(sr, 15.0f); gain[c].snap(1.0f);
            meter[c].configure(sr);
        }
        eq.configure(sr);
        al.configure(sr);
        delay.configure(sr);
        loud.configure(sr);
    }
    void update(const BusParams& p, float sr)
    {
        eq.update(p.eq, sr);
        al.set(p.al_target.load(std::memory_order_relaxed),
               p.al_max_boost.load(std::memory_order_relaxed),
               p.al_max_cut.load(std::memory_order_relaxed));
        al.set_enabled(p.al_on.load(std::memory_order_relaxed) != 0);
    }
};

// ---------------------------------------------------------------------------
struct Engine;

enum EpKind { kEpHwIn, kEpVirtSink, kEpHwOut, kEpVirtSource, kEpVbanOut, kEpCableSink };

struct Endpoint {
    Engine*     eng = nullptr;
    EpKind      kind;
    int         index = 0;          // strip index or bus index
    pw_stream*  stream = nullptr;
    spa_hook    listener = {};
    Ring*       ring = nullptr;
    std::string node_name, desc, target;
    bool        connected = false;
    int         vban_bus = 0;      // kEpVbanOut: which bus feeds this sender
    uint32_t    nchan = kChan;    // negotiated channel count
    uint32_t    nrate = kRate;
    // kEpHwOut only: the bus mode this stream was connected for, and the state
    // the wider modes need. A change of mode changes the channel count, so it
    // is a reconnect rather than a parameter.
    int         mode = kBusNormal;
    Upmix       upmix;
};

struct Engine {
    pw_main_loop* loop = nullptr;
    pw_context*   ctx  = nullptr;
    pw_core*      core = nullptr;
    pw_registry*  registry = nullptr;
    spa_hook      registry_listener = {};
    spa_hook      core_listener = {};
    spa_source*   timer = nullptr;

    // The registry's first snapshot is only complete once a core sync comes
    // back; nothing decides "that node does not exist" before then.
    int           sync_seq = -1;
    bool          synced = false;

    // The screen-share sink (kStreamSinkName), created by the engine rather
    // than by a PipeWire config file, so it exists without a log-out and goes
    // away with the engine. See stream_sink_tick().
    pw_proxy*     sink_proxy = nullptr;
    spa_hook      sink_listener = {};
    uint32_t      sink_id = SPA_ID_INVALID;     // our node's global id, once bound
    bool          sink_dead = false;            // errored or removed: drop the proxy
    double        sink_retry_at = 0.0;
    double        sink_backoff = 1.0;
    uint32_t      sink_seen = SPA_ID_INVALID;   // the sink node buses were last wired to
    // A bus pointed at the stream sink is not connected until the sink exists,
    // and carries node.dont-fallback: otherwise the session manager links it to
    // the default device, and the viewers' mix plays in your headphones.
    bool          bus_waiting[kPhysBuses] = {};

    // The stream guard (engine/streamguard.h). The registry keeps this view of
    // the graph current; guard_work() plans from it and applies the plan.
    std::map<uint32_t, PortInfo>    ports;
    std::map<uint32_t, LinkInfo>    links;
    std::map<uint32_t, std::string> capture_target;     // capture node -> target.object
    // Capture nodes are bound to read target.object, which their registry
    // global does not carry. Until it arrives the guard waits (briefly): with
    // the target unknown it would take the window-share fallback and feed the
    // bus into every capture - the chamber, for a fraction of a second.
    struct BoundCapture {
        Engine*   e = nullptr;
        uint32_t  id = 0;
        pw_proxy* proxy = nullptr;
        spa_hook  hook = {};
        double    since = 0.0;
    };
    std::map<uint32_t, std::unique_ptr<BoundCapture>> bound_caps;
    // A link asked for but not yet in the registry. Without remembering it, a
    // plan made in the gap would ask again, and two links from the bus into one
    // capture is the very doubling the guard exists to prevent.
    struct PendingLink {
        Engine*   e = nullptr;
        pw_proxy* proxy = nullptr;
        spa_hook  hook = {};
        uint32_t  out = 0, in = 0;
        double    expires = 0.0;
        bool      done = false;
    };
    std::vector<std::unique_ptr<PendingLink>> pending_make;
    std::map<uint32_t, double> pending_drop;            // link id -> give up at
    // Drops per (source node, capture node), to notice the session manager
    // putting a link straight back - and to stop turning that into a busy loop.
    std::map<std::pair<uint32_t, uint32_t>, std::deque<double>> drop_hist;
    spa_source* guard_timer = nullptr;
    bool        guard_armed = false;
    double      guard_safety_at = 0.0;
    int         guard_sig = -1;
    int         guard_logged_state = -1;

    // Every live node by id, with the description and media class that let a
    // saved preset find its device again - see nodes.h.
    NodeMap nodes;

    Shared*  shm = nullptr;
    int      shm_fd = -1;

    Ring     strip_ring[kStrips];
    Ring     bus_ring[kBuses];
    Endpoint ep_in[kStrips];
    Endpoint ep_out[kBuses];

    StripDsp sdsp[kStrips];
    BusDsp   bdsp[kBuses];

    // Spectrum analyser: the mixer taps one signal into spec_tap, and the
    // control thread transforms it. Only one at a time, because only one EQ
    // editor is ever looking.
    SpecTap          spec_tap;
    SpectrumAnalyzer spec_an;
    float            spec_win[kSpecFft] = {};
    int              spec_src = kSpecNone;      // what the mixer is tapping now
    spa_source*      spec_timer = nullptr;

    float sr = (float)kRate;
    uint32_t routing_seen = 0;
    // Last explicit-route request counter seen per endpoint, so a re-issued
    // route rebuilds the stream instead of comparing equal and being dropped.
    uint32_t hw_gen_seen[kHwStrips]   = {};
    uint32_t bus_gen_seen[kPhysBuses] = {};
    uint32_t cmd_seen = 0;
    bool in_mix = false;

    // Tape deck
    // Virtual cables: extra sinks that any application can play into, each
    // feeding whichever hardware strip it is assigned to.
    Endpoint cable_ep[kCables];
    std::atomic<int> cable_target[kCables];   // strip index, -1 when unassigned

    Ring vban_ring[kVbanStreams];        // mixer -> one VBAN sender each
    Endpoint vban_ep[kVbanStreams];
    Ring rec_ring;                       // mixer -> writer thread
    Ring play_ring;                      // reader thread -> mixer
    std::thread rec_thread, play_thread;
    std::atomic<bool> rec_run{false}, play_run{false};
    std::atomic<uint32_t> rec_dropped{0};

    // VBAN: one PipeWire module per enabled stream, reloaded when its
    // configuration string changes.
    pw_impl_module* vban_out_mod[kVbanStreams] = {};
    pw_impl_module* vban_in_mod [kVbanStreams] = {};
    std::string     vban_out_args[kVbanStreams];
    std::string     vban_in_args [kVbanStreams];
    uint32_t        vban_seen = 0;

    // Ducker
    float duck_env = 0.0f;                 // 0..1
    SmoothGain duck_gain[kStrips];

    // Pre-fader send gains, one per strip per bus (engine/matrix.h).
    StripSends ssend[kStrips];

    // The timing-test click. One phase for every bus, so a tick leaves them all
    // on the same sample and the only difference left to hear is the one being
    // measured.
    ClickTrain click;
    time_t     click_since = 0;            // when the mask last went non-zero

    // scratch
    float stripout[kStrips][kMaxChunk * kChan];   // per-strip post-DSP output
    float stripsend[kStrips][kMaxChunk * kChan];  // the same, before the fader
    float dgbuf[kMaxChunk];                       // one strip's duck gain per frame
    float sbuf[kMaxChunk * kChan];
    float pbuf[kMaxChunk * kChan];
    float acc[kBuses][kMaxChunk * kChan];

    void start_record();
    void stop_record();
    void start_play();
    void stop_play();
    void apply_vban();

    void mix_chunk(uint32_t n);
    void ensure_ring(Ring& r, uint32_t n);
    void poll_control();
    void poll_spectrum();
    void stream_sink_tick();
    void schedule_guard();
    void guard_work();
};

static Engine g_eng;
static volatile sig_atomic_t g_run = 1;

static double now_s();

// A capture node's info: the target.object that says which stream it records.
static void on_capture_info(void* data, const pw_node_info* info)
{
    auto* bc = static_cast<Engine::BoundCapture*>(data);
    if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) return;
    const char* t = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT);
    bc->e->capture_target[bc->id] = t ? t : "";
    bc->e->schedule_guard();
}

static const pw_node_events kCaptureEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = on_capture_info,
};

static uint32_t dict_u32(const spa_dict* d, const char* key)
{
    const char* v = spa_dict_lookup(d, key);
    return v ? (uint32_t)std::strtoul(v, nullptr, 10) : 0;
}

static void on_registry_global(void* data, uint32_t id, uint32_t /*permissions*/,
                               const char* type, uint32_t /*version*/,
                               const spa_dict* props)
{
    if (!props) return;
    Engine* e = static_cast<Engine*>(data);

    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char* dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char* mon = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
        e->ports[id] = { dict_u32(props, PW_KEY_NODE_ID),
                         dir && std::strcmp(dir, "in") == 0,
                         mon && std::strcmp(mon, "true") == 0,
                         port_channel(spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL),
                                      spa_dict_lookup(props, PW_KEY_PORT_NAME)) };
        e->schedule_guard();
        return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const LinkInfo l = { dict_u32(props, PW_KEY_LINK_OUTPUT_PORT),
                             dict_u32(props, PW_KEY_LINK_INPUT_PORT) };
        e->links[id] = l;
        for (auto& pl : e->pending_make)
            if (pl->out == l.out_port && pl->in == l.in_port) pl->done = true;
        e->schedule_guard();
        return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name) return;
    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char* ser = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    e->nodes[id] = { name, desc ? desc : "", cls ? cls : "",
                     ser ? std::strtoull(ser, nullptr, 10) : 0 };

    if (std::strcmp(name, kCaptureName) == 0 && !e->bound_caps.count(id)) {
        auto* proxy = static_cast<pw_proxy*>(
            pw_registry_bind(e->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (proxy) {
            auto bc = std::make_unique<Engine::BoundCapture>();
            bc->e = e; bc->id = id; bc->proxy = proxy; bc->since = now_s();
            pw_node_add_listener(reinterpret_cast<pw_node*>(proxy), &bc->hook,
                                 &kCaptureEvents, bc.get());
            e->bound_caps[id] = std::move(bc);
        }
    }
    e->schedule_guard();
}

static void on_registry_global_remove(void* data, uint32_t id)
{
    Engine* e = static_cast<Engine*>(data);
    e->nodes.erase(id);
    e->ports.erase(id);
    e->links.erase(id);
    e->pending_drop.erase(id);
    e->capture_target.erase(id);
    auto it = e->bound_caps.find(id);
    if (it != e->bound_caps.end()) {
        spa_hook_remove(&it->second->hook);
        pw_proxy_destroy(it->second->proxy);
        e->bound_caps.erase(it);
    }
    e->schedule_guard();
}

static const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void on_core_done(void* data, uint32_t id, int seq)
{
    Engine* e = static_cast<Engine*>(data);
    if (id == PW_ID_CORE && seq == e->sync_seq) e->synced = true;
}

static void on_core_error(void* data, uint32_t id, int /*seq*/, int res, const char* message)
{
    Engine* e = static_cast<Engine*>(data);
    if (e->sink_proxy && id == pw_proxy_get_id(e->sink_proxy)) {
        std::fprintf(stderr, "[bb] stream sink: %s (%s)\n", message ? message : "error",
                     spa_strerror(res));
        e->sink_dead = true;
    }
}

static const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

// Events on our stream sink. Nothing is destroyed in here - the proxy is
// dropped on the next control tick, outside its own callbacks.
static void on_sink_bound_props(void* data, uint32_t global_id, const spa_dict* /*props*/)
{
    static_cast<Engine*>(data)->sink_id = global_id;
}
static void on_sink_removed(void* data) { static_cast<Engine*>(data)->sink_dead = true; }
static void on_sink_error(void* data, int /*seq*/, int res, const char* message)
{
    std::fprintf(stderr, "[bb] stream sink: %s (%s)\n", message ? message : "error",
                 spa_strerror(res));
    static_cast<Engine*>(data)->sink_dead = true;
}

static const pw_proxy_events kSinkProxyEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .removed = on_sink_removed,
    .error = on_sink_error,
    .bound_props = on_sink_bound_props,
};

static double now_s()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + 1e-9 * double(t.tv_nsec);
}

// If the saved node.name is gone but we know what the device was called, look
// for a live device of the same direction advertising the same description.
static std::string resolve_device(Engine* e, const std::string& name, const char* desc,
                                  Direction dir)
{
    if (name.rfind(kCablePrefix, 0) == 0) return name;      // virtual cable
    const std::string r = resolve_node(e->nodes, name, desc ? desc : "", dir);
    if (r != name)
        std::fprintf(stderr, "[bb] '%s' is gone; matched '%s' by description \"%s\"\n",
                     name.c_str(), r.c_str(), desc);
    return r;
}

// ---------------------------------------------------------------------------
// The mixer. Consumes n frames from every strip ring, produces n frames into
// every bus ring. Runs on the data-loop thread only.
// ---------------------------------------------------------------------------
void Engine::mix_chunk(uint32_t n)
{
    if (n > kMaxChunk) n = kMaxChunk;
    Shared* s = shm;
    // The quantum was stored once as 1024 at startup and never touched again -
    // a gauge that reported a constant, which is worse than no gauge. It is the
    // real block size now, and the latency arithmetic below depends on it.
    if (n && s->quantum.load(std::memory_order_relaxed) != n)
        s->quantum.store(n, std::memory_order_relaxed);
    // clock_gettime is a vDSO read, tens of nanoseconds, and this is per block
    // rather than per sample - cheap enough to leave on always.
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int b = 0; b < kBuses; ++b)
        std::memset(acc[b], 0, sizeof(float) * n * kChan);

    // Which buses take strips before the fader (BusParams::prefader).
    bool prefader[kBuses];
    for (int b = 0; b < kBuses; ++b)
        prefader[b] = s->bus[b].prefader.load(std::memory_order_relaxed) != 0;

    // ---- pass 1: run each strip's DSP into its own buffer -----------------
    // Keeping the per-strip result lets the ducker see every key strip before
    // anything is summed, and lets solo be decided per bus.
    float key_peak = 0.0f;
    bool need_pre[kStrips] = {};
    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        StripDsp& d = sdsp[i];
        for (int b = 0; b < kBuses; ++b)
            if (prefader[b] && p.bus_on[b].load(std::memory_order_relaxed)) need_pre[i] = true;
        float* const tap = need_pre[i] ? stripsend[i] : nullptr;

        strip_ring[i].drop_to(kResyncQuanta * n);
        strip_ring[i].read_padded(sbuf, n);
        d.update(p, sr);

        const bool mono_src = p.mono_source.load(std::memory_order_relaxed) != 0;
        const bool mono     = p.mono.load(std::memory_order_relaxed) != 0;
        const bool muted    = p.mute.load(std::memory_order_relaxed) != 0;
        const float glin    = db_to_lin(p.gain_db.load(std::memory_order_relaxed));
        float pl, pr; pan_gains(p.pan_x.load(std::memory_order_relaxed), pl, pr);

        const bool par_on   = p.eq.on.load(std::memory_order_relaxed) != 0;
        const bool fx_on    = p.fx.on.load(std::memory_order_relaxed) != 0;
        for (int c = 0; c < kChan; ++c) d.gain[c].set_target(muted ? 0.0f : glin);

        float pre_pk[kChan] = {0, 0}, post_pk[kChan] = {0, 0};

        for (uint32_t f = 0; f < n; ++f) {
            float L = sbuf[f * kChan], R = sbuf[f * kChan + 1];
            if (mono_src) R = L;
            if (mono)     { const float m = 0.5f * (L + R); L = R = m; }

            float ch[kChan] = { L, R };
            for (int c = 0; c < kChan; ++c) {
                float x = ch[c];
                const float a = std::fabs(x);
                if (a > pre_pk[c]) pre_pk[c] = a;

                x = d.gate[c].process(x);
                x = d.comp[c].process(x);
                x = d.eq_lo[c].process(x);
                x = d.eq_mid[c].process(x);
                x = d.eq_hi[c].process(x);
                x = d.aud[c].process(x);
                // The parametric block sits after the three tone knobs and
                // before the fader, so its preamp trims the EQ rather than the
                // level you set by hand.
                if (par_on) x = d.par.process(c, x);
                // The voice changer sits after the EQ, so the EQ cleans the
                // real voice going in rather than the artefacts coming out,
                // and before the fader, so the fader still means level.
                if (fx_on)  x = d.fx.process(c, x);
                // Pan first, then the fader, so the pre-fader tap carries the
                // strip's placement as well as its processing.
                x *= (c == 0 ? pl : pr) * 1.41421356f;   // pan law is unity at centre
                if (tap) tap[f * kChan + c] = x;
                x *= d.gain[c].next();

                const float b2 = std::fabs(x);
                if (b2 > post_pk[c]) post_pk[c] = b2;
                ch[c] = x;
            }
            stripout[i][f * kChan]     = ch[0];
            stripout[i][f * kChan + 1] = ch[1];
        }

        // A key strip drives the ducker from its post-processing level, so the
        // gate and compressor decide what counts as speech. Read BEFORE the
        // delay below: a strip held back for lip-sync must not duck late too.
        if (p.duck_key.load(std::memory_order_relaxed))
            key_peak = std::max(key_peak, std::max(post_pk[0], post_pk[1]));

        // Time alignment, last of all, so every bus this strip feeds gets the
        // same held-back signal. The meters above are deliberately pre-delay:
        // they report what the strip produced, and a delay is transport, not
        // level - a meter that lagged the fader by a quarter of a second would
        // read as the mixer being broken.
        const float dms = p.delay_ms.load(std::memory_order_relaxed);
        d.delay.set_ms(dms);
        if (d.delay.active()) d.delay.process(stripout[i], (int)n);
        if (tap) {
            // A tap that was idle holds whatever it last carried: start clean.
            if (!d.send_live) d.send_delay.reset();
            d.send_delay.set_ms(dms);
            if (d.send_delay.active()) d.send_delay.process(tap, (int)n);
        }
        d.send_live = tap != nullptr;

        for (int c = 0; c < kChan; ++c) {
            d.pre[c].feed_peak(pre_pk[c], n);
            d.post[c].feed_peak(post_pk[c], n);
            s->meters.strip_pre [i][c].store(d.pre[c].peak,  std::memory_order_relaxed);
            s->meters.strip_post[i][c].store(d.post[c].peak, std::memory_order_relaxed);
            if (post_pk[c] >= 0.999f) s->meters.strip_clip[i].store(1, std::memory_order_relaxed);
        }
        s->meters.strip_gate_gain[i].store(d.gate[0].gain, std::memory_order_relaxed);
        s->meters.strip_comp_gr[i].store(d.comp[0].gr_db,  std::memory_order_relaxed);
    }

    // ---- ducker envelope --------------------------------------------------
    {
        const bool on = s->duck_enabled.load(std::memory_order_relaxed) != 0;
        const float thr = db_to_lin(s->duck_threshold_db.load(std::memory_order_relaxed));
        const float target = (on && key_peak > thr) ? 1.0f : 0.0f;
        const float ms = (target > duck_env)
                           ? s->duck_attack_ms.load(std::memory_order_relaxed)
                           : s->duck_release_ms.load(std::memory_order_relaxed);
        // One-pole over the whole block; ducking timings are tens of
        // milliseconds, so per-block resolution is inaudible.
        const float coeff = std::exp(-float(n) / (std::max(ms, 1.0f) * 0.001f * sr));
        duck_env = target + (duck_env - target) * coeff;
        s->meters.duck_env.store(duck_env, std::memory_order_relaxed);
    }

    // ---- pass 2: apply ducking and sum into the buses ----------------------
    // Solo is decided per bus: a soloed strip silences the others only on the
    // buses it actually feeds. Pre-fader buses are exempt: solo is for
    // listening, and soloing a source in your headphones must not cut it - or
    // everything else - out of what a stream is hearing.
    bool solo_on_bus[kBuses] = {};
    for (int b = 0; b < kBuses; ++b) {
        if (prefader[b]) continue;
        for (int i = 0; i < kStrips; ++i)
            if (s->strip[i].solo.load(std::memory_order_relaxed) &&
                s->strip[i].bus_on[b].load(std::memory_order_relaxed)) {
                solo_on_bus[b] = true;
                break;
            }
    }

    float* const accp[kBuses] = { acc[0], acc[1], acc[2], acc[3], acc[4] };
    static_assert(kBuses == 5, "accp lists every bus");
    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        const float depth = p.duck_depth_db.load(std::memory_order_relaxed);
        const float duck_db = depth * duck_env;
        s->meters.strip_duck_gr[i].store(duck_db, std::memory_order_relaxed);
        duck_gain[i].set_target(db_to_lin(duck_db));
        // Once per frame, however many buses the strip feeds.
        for (uint32_t f = 0; f < n; ++f) dgbuf[f] = duck_gain[i].next();

        const bool soloed = p.solo.load(std::memory_order_relaxed) != 0;
        const bool muted  = p.mute.load(std::memory_order_relaxed) != 0;
        BusRoute route[kBuses];
        float target[kBuses];
        for (int b = 0; b < kBuses; ++b) {
            route[b].on = p.bus_on[b].load(std::memory_order_relaxed) != 0;
            route[b].prefader = prefader[b];
            route[b].solo_blocked = solo_on_bus[b] && !soloed;
            target[b] = send_target(p.send_db[b].load(std::memory_order_relaxed), muted);
        }
        matrix_add_strip(accp, stripout[i], need_pre[i] ? stripsend[i] : nullptr,
                         dgbuf, n, route, target, ssend[i]);
    }

    // Tape deck playback feeds the matrix like any other source, before the
    // bus stage, so bus EQ and gain apply to it too.
    if (s->rec.state.load(std::memory_order_relaxed) == kRecPlaying) {
        play_ring.read_padded(pbuf, n);
        const float pg = db_to_lin(s->rec.gain_db.load(std::memory_order_relaxed));
        for (int b = 0; b < kBuses; ++b) {
            if (!s->rec.bus_on[b].load(std::memory_order_relaxed)) continue;
            for (uint32_t f = 0; f < n * kChan; ++f) acc[b][f] += pbuf[f] * pg;
        }
    }

    // The timing-test click, if one is running. Read once for the whole cycle
    // so every bus ticks off the same mask and the same phase.
    const int click_mask = s->click_mask.load(std::memory_order_relaxed);
    click.set_running(click_mask != 0);

    // Bus stage: mono -> EQ -> auto-level -> gain -> limiter.
    for (int b = 0; b < kBuses; ++b) {
        BusParams& p = s->bus[b];
        BusDsp& d = bdsp[b];
        d.update(p, sr);

        // Auto-level holds while the ducker is pulling down something on this
        // bus: ducked music is quieter on purpose, and riding it back up would
        // undo the ducker within a few seconds of talking.
        bool ducking = false;
        if (duck_env > 0.05f)
            for (int i = 0; i < kStrips && !ducking; ++i)
                ducking = s->strip[i].bus_on[b].load(std::memory_order_relaxed) &&
                          s->strip[i].duck_depth_db.load(std::memory_order_relaxed) < 0.0f;
        d.al.set_hold(ducking);
        const bool al_live = d.al.live();

        const bool eq_on = p.eq.on.load(std::memory_order_relaxed) != 0;
        const bool mono  = p.mono.load(std::memory_order_relaxed) != 0;
        const bool muted = p.mute.load(std::memory_order_relaxed) != 0;
        const float glin = db_to_lin(p.gain_db.load(std::memory_order_relaxed));
        for (int c = 0; c < kChan; ++c) d.gain[c].set_target(muted ? 0.0f : glin);

        float pk[kChan] = {0, 0};
        for (uint32_t f = 0; f < n; ++f) {
            float L = acc[b][f * kChan], R = acc[b][f * kChan + 1];
            if (mono) { const float m = 0.5f * (L + R); L = R = m; }
            if (eq_on) { L = d.eq.process(0, L); R = d.eq.process(1, R); }
            // One gain for both channels, so the stereo image cannot wander.
            if (al_live) { const float ag = d.al.frame(L, R); L *= ag; R *= ag; }
            float ch[kChan] = { L, R };
            for (int c = 0; c < kChan; ++c) {
                float x = ch[c];
                x *= d.gain[c].next();
                // Safety limiter: transparent below -3 dBFS, soft-knee above,
                // asymptotic to full scale so a hot matrix can never wrap.
                const float ax = std::fabs(x);
                if (ax > 0.7f) {
                    const float sgn = x < 0.0f ? -1.0f : 1.0f;
                    x = sgn * (0.7f + 0.3f * std::tanh((ax - 0.7f) / 0.3f));
                }
                const float a = std::fabs(x);
                if (a > pk[c]) pk[c] = a;
                ch[c] = x;
            }
            acc[b][f * kChan] = ch[0]; acc[b][f * kChan + 1] = ch[1];
        }
        for (int c = 0; c < kChan; ++c) {
            d.meter[c].feed_peak(pk[c], n);
            s->meters.bus_out[b][c].store(d.meter[c].peak, std::memory_order_relaxed);
            if (pk[c] >= 0.999f) s->meters.bus_clip[b].store(1, std::memory_order_relaxed);
        }
        s->meters.bus_al_db[b].store(d.al.gain_db(), std::memory_order_relaxed);
        s->meters.bus_al_state[b].store(al_live ? d.al.state() : (int)kAlOff,
                                        std::memory_order_relaxed);

        // The test tick goes in after the fader and the limiter but before the
        // delay, so it travels exactly the path being aligned - and a bus you
        // have turned down still ticks, which is rather the point of being able
        // to test one. It lands after the peak meters and before the loudness
        // meter; a 3 ms tick at -20 dBFS every 700 ms is 0.4% duty and moves
        // LUFS by hundredths, so it is not worth a second code path.
        if (b < kPhysBuses && (click_mask & (1 << b))) click.mix(acc[b], n);

        // Alignment goes last, after the limiter, so the delay carries exactly
        // what the device will receive. This is the one that matters for
        // listening: two outputs almost never have the same latency, and only a
        // delay here can line them up - a strip feeds both, so delaying a strip
        // moves both together.
        d.delay.set_ms(p.delay_ms.load(std::memory_order_relaxed));
        if (d.delay.active()) d.delay.process(acc[b], (int)n);

        // Loudness measured on what actually leaves the bus. Peak says whether
        // it will clip; this says how loud it will sound, which is the number
        // every streaming platform measures.
        d.loud.process(acc[b], (int)n);
        s->meters.bus_lufs_s[b].store(d.loud.short_term(), std::memory_order_relaxed);
        s->meters.bus_lufs_i[b].store(d.loud.integrated(), std::memory_order_relaxed);

        bus_ring[b].drop_to(kResyncQuanta * n);
        bus_ring[b].write(acc[b], n);
    }
    click.advance(n);

    // Spectrum tap: whichever single signal an open EQ editor asked for. Buses
    // are tapped post-EQ, strips post-processing, so what is drawn is what the
    // meter next to it is showing.
    {
        const int src = s->spec.source.load(std::memory_order_relaxed);
        if (src != spec_src) {          // switched editors: do not show the old signal
            spec_tap.clear();
            spec_src = src;
        }
        if (src >= 0 && src < kBuses)                 spec_tap.write_stereo(acc[src], n);
        else if (src >= kBuses && src < kSpecSourceCount)
            spec_tap.write_stereo(stripout[src - kBuses], n);
    }

    // VBAN senders each get their own ring so they never contend with the
    // bus endpoint for the same single-consumer buffer.
    for (int i = 0; i < kVbanStreams; ++i) {
        if (!vban_ep[i].stream) continue;
        const int src = clampi(vban_ep[i].vban_bus, 0, kBuses - 1);
        vban_ring[i].drop_to(kResyncQuanta * n);
        vban_ring[i].write(acc[src], n);
    }

    // Record tap: the selected bus, post everything, exactly as it leaves.
    if (s->rec.state.load(std::memory_order_relaxed) == kRecRecording) {
        const int src = clampi(s->rec.source_bus.load(std::memory_order_relaxed), 0, kBuses - 1);
        // If the writer thread stalls, drop the oldest audio rather than
        // corrupting the ring, and count it so the GUI can report it.
        if (rec_ring.avail() + n > kRingFrames - 64) {
            rec_ring.drop_to(kRingFrames / 2);
            rec_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        rec_ring.write(acc[src], n);
    }
    s->engine_heartbeat.fetch_add(1, std::memory_order_relaxed);

    // Time spent as a fraction of the time this block represents: 1000 means
    // the mixer took exactly as long as the audio it produced, and anything
    // approaching that will drop out.
    timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double spent = double(t1.tv_sec - t0.tv_sec)
                       + 1e-9 * double(t1.tv_nsec - t0.tv_nsec);
    const double budget = double(n) / double(sr);
    if (budget > 0.0) {
        const uint32_t pm = (uint32_t)(spent / budget * 1000.0);
        const uint32_t prev = s->dsp_load.load(std::memory_order_relaxed);
        // Rise instantly, fall about 1.5% a block, so a brief spike is visible
        // for a moment rather than for a frame.
        s->dsp_load.store(pm > prev ? pm : (prev * 63) / 64,
                          std::memory_order_relaxed);
    }
}

// Runs the mixer until `r` holds at least n frames. Every output endpoint
// calls this; whichever runs first in the cycle does the work.
void Engine::ensure_ring(Ring& r, uint32_t n)
{
    if (in_mix) return;
    in_mix = true;
    int guard = 0;
    while (r.avail() < n && guard++ < 8) {
        uint32_t want = n - r.avail();
        if (want > kMaxChunk) want = kMaxChunk;
        mix_chunk(want);
    }
    in_mix = false;
}

// ---------------------------------------------------------------------------
// pw_stream callbacks
// ---------------------------------------------------------------------------
static void on_process(void* data)
{
    Endpoint* e = static_cast<Endpoint*>(data);
    Engine* E = e->eng;
    pw_buffer* b = pw_stream_dequeue_buffer(e->stream);
    if (!b) return;
    spa_data& sd = b->buffer->datas[0];

    const bool is_input = (e->kind == kEpHwIn || e->kind == kEpVirtSink ||
                          e->kind == kEpCableSink);

    // A cable writes into whichever strip currently claims it; unassigned, its
    // audio is simply discarded.
    int ring_idx = e->index;
    if (e->kind == kEpCableSink) {
        ring_idx = E->cable_target[e->index].load(std::memory_order_relaxed);
        if (ring_idx < 0) { pw_stream_queue_buffer(e->stream, b); return; }
    }

    if (is_input) {
        const float* in = static_cast<const float*>(sd.data);
        const uint32_t nc = e->nchan ? e->nchan : kChan;
        const uint32_t n = in ? sd.chunk->size / (sizeof(float) * nc) : 0;
        if (n) {
            if (nc == kChan) {
                E->strip_ring[ring_idx].write(in, n);
            } else {
                // Mono capture (or an unexpected layout): fold channel 0 across
                // both, in bounded chunks so the scratch buffer is never over-run.
                static thread_local float tmp[kMaxChunk * kChan];
                for (uint32_t off = 0; off < n; off += kMaxChunk) {
                    const uint32_t m = (n - off) > kMaxChunk ? kMaxChunk : (n - off);
                    for (uint32_t f = 0; f < m; ++f) {
                        const float v = in[(off + f) * nc];
                        tmp[f * kChan] = v; tmp[f * kChan + 1] = v;
                    }
                    E->strip_ring[ring_idx].write(tmp, m);
                }
            }
            E->shm->strip[ring_idx].present.store(1, std::memory_order_relaxed);
        }
    } else {
        const uint32_t nc = e->nchan ? e->nchan : kChan;
        uint32_t n = b->requested ? (uint32_t)b->requested
                                  : sd.maxsize / (sizeof(float) * nc);
        const uint32_t cap = sd.maxsize / (sizeof(float) * nc);
        if (n > cap) n = cap;
        float* out = static_cast<float*>(sd.data);
        Ring& ring = (e->kind == kEpVbanOut) ? E->vban_ring[e->index] : E->bus_ring[e->index];
        if (out) {
            E->ensure_ring(ring, n);
            const int mode = (e->kind == kEpHwOut) ? e->mode : kBusNormal;
            const uint32_t want = (uint32_t)bus_layout(mode).channels;
            if (mode != kBusNormal && nc == want) {
                // The bus itself is stereo whatever the mode; the mode is what
                // makes it wider, and it happens here at the very last moment.
                static thread_local float tmp[kMaxChunk * kChan];
                for (uint32_t off = 0; off < n; off += kMaxChunk) {
                    const uint32_t m = (n - off) > kMaxChunk ? kMaxChunk : (n - off);
                    ring.read_padded(tmp, m);
                    e->upmix.process(mode, tmp, out + (size_t)off * nc, (int)m);
                }
            } else if (nc == kChan) {
                ring.read_padded(out, n);
            } else {
                static thread_local float tmp[kMaxChunk * kChan];
                for (uint32_t off = 0; off < n; off += kMaxChunk) {
                    const uint32_t m = (n - off) > kMaxChunk ? kMaxChunk : (n - off);
                    ring.read_padded(tmp, m);
                    for (uint32_t f = 0; f < m; ++f) {
                        const float v = 0.5f * (tmp[f * kChan] + tmp[f * kChan + 1]);
                        for (uint32_t c = 0; c < nc; ++c) out[(off + f) * nc + c] = v;
                    }
                }
            }
        }
        sd.chunk->offset = 0;
        sd.chunk->stride = sizeof(float) * nc;
        sd.chunk->size   = n * nc * sizeof(float);
    }
    pw_stream_queue_buffer(e->stream, b);
}

static void on_state(void* data, pw_stream_state old, pw_stream_state st, const char* err)
{
    Endpoint* e = static_cast<Endpoint*>(data);
    if (err) std::fprintf(stderr, "[bb] %s: %s -> %s (%s)\n", e->desc.c_str(),
                          pw_stream_state_as_string(old), pw_stream_state_as_string(st), err);
    if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED)
        if (e->kind == kEpHwIn) e->eng->shm->strip[e->index].present.store(0, std::memory_order_relaxed);
}

// How far behind a device is, in milliseconds, from what PipeWire reports for
// it. Three parts, any of which may be zero: a share of the graph quantum, a
// number of frames, and a flat time. For a Bluetooth sink the last one carries
// the codec and link delay, which is the part nothing else can see and the
// whole reason this is read rather than guessed at.
static float latency_ms_of(const spa_latency_info& in, uint32_t quantum, uint32_t rate)
{
    if (!rate) return -1.0f;
    const double frames = (double)in.min_quantum * (double)quantum + (double)in.min_rate;
    return (float)(frames * 1000.0 / (double)rate + (double)in.min_ns / 1e6);
}

static void on_param_changed(void* data, uint32_t id, const spa_pod* param)
{
    Endpoint* e = static_cast<Endpoint*>(data);
    if (param && id == SPA_PARAM_Latency) {
        spa_latency_info info = {};
        if (spa_latency_parse(param, &info) < 0) return;
        Engine* E = e->eng;
        if (!E || !E->shm) return;
        const uint32_t q = E->shm->quantum.load(std::memory_order_relaxed);
        const float ms = latency_ms_of(info, q ? q : 1024, e->nrate ? e->nrate : kRate);
        if (ms < 0.0f) return;
        // Both directions are reported and only one of them carries the figure
        // that matters, which differs between a capture stream and a playback
        // one. Keeping the larger of the two is what makes this work for both
        // without having to know which is which.
        if (e->kind == kEpHwOut && e->index >= 0 && e->index < kPhysBuses) {
            const float was = E->shm->out_latency_ms[e->index].load(std::memory_order_relaxed);
            if (ms > was) E->shm->out_latency_ms[e->index].store(ms, std::memory_order_relaxed);
        } else if (e->kind == kEpHwIn && e->index >= 0 && e->index < kHwStrips) {
            const float was = E->shm->in_latency_ms[e->index].load(std::memory_order_relaxed);
            if (ms > was) E->shm->in_latency_ms[e->index].store(ms, std::memory_order_relaxed);
        }
        return;
    }
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mtype = 0, mstype = 0;
    if (spa_format_parse(param, &mtype, &mstype) < 0) return;
    if (mtype != SPA_MEDIA_TYPE_audio || mstype != SPA_MEDIA_SUBTYPE_raw) return;
    spa_audio_info_raw info = {};
    if (spa_format_audio_raw_parse(param, &info) < 0) return;
    e->nchan = info.channels ? info.channels : kChan;
    e->nrate = info.rate ? info.rate : kRate;
    std::fprintf(stderr, "[bb] %s negotiated %u ch @ %u Hz\n",
                 e->desc.c_str(), e->nchan, e->nrate);
}

static const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .param_changed = on_param_changed,
    .process = on_process,
};

// Our channel ids are PipeWire-free so the DSP can be tested on its own; this
// is the only place that has to know about SPA.
static uint32_t spa_position_for(int id)
{
    switch (id) {
    case kChFL:  return SPA_AUDIO_CHANNEL_FL;
    case kChFR:  return SPA_AUDIO_CHANNEL_FR;
    case kChFC:  return SPA_AUDIO_CHANNEL_FC;
    case kChLFE: return SPA_AUDIO_CHANNEL_LFE;
    case kChBL:  return SPA_AUDIO_CHANNEL_RL;
    case kChBR:  return SPA_AUDIO_CHANNEL_RR;
    case kChSL:  return SPA_AUDIO_CHANNEL_SL;
    case kChSR:  return SPA_AUDIO_CHANNEL_SR;
    }
    return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static bool connect_endpoint(Engine* E, Endpoint* e)
{
    if (e->stream) { pw_stream_destroy(e->stream); e->stream = nullptr; e->connected = false; }

    // Must match the kinds treated as inputs in on_process: declaring
    // media.class=Audio/Sink while connecting as an output produces a
    // contradictory node and crashes audioconvert during negotiation.
    const bool is_input = (e->kind == kEpHwIn || e->kind == kEpVirtSink ||
                           e->kind == kEpCableSink);
    const char* media_class =
        e->kind == kEpHwIn       ? "Stream/Input/Audio"  :
        e->kind == kEpVirtSink   ? "Audio/Sink"          :
        e->kind == kEpCableSink  ? "Audio/Sink"          :
        e->kind == kEpHwOut      ? "Stream/Output/Audio" :
        e->kind == kEpVbanOut    ? "Stream/Output/Audio" : "Audio/Source";

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,          "Audio",
        PW_KEY_MEDIA_CATEGORY,      is_input ? "Capture" : "Playback",
        PW_KEY_MEDIA_CLASS,         media_class,
        PW_KEY_MEDIA_ROLE,          "Production",
        PW_KEY_APP_NAME,            "BetterBanana",
        PW_KEY_NODE_NAME,           e->node_name.c_str(),
        PW_KEY_NODE_DESCRIPTION,    e->desc.c_str(),
        PW_KEY_NODE_GROUP,          "betterbanana",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        // Ask for small blocks ourselves. Without this the graph runs at
        // PipeWire's default of 1024 frames - 21 ms a block, and the mixer
        // holds about two - unless some other application happens to want
        // less. Discord's voice engine does, so monitoring your own mic felt
        // tight during a call and laggy outside one. 256 at 48 kHz is 5.3 ms.
        PW_KEY_NODE_LATENCY,        "256/48000",
        // WirePlumber keys its saved stream volumes on the first of
        // application.id / application.name / media.name / node.name that a node
        // carries, so the application.name above collapses every endpoint into a
        // single stored entry: the strips and cables all came back at whichever
        // fader happened to move last. Fader state belongs to the preset, so opt
        // out of WirePlumber's restore rather than fight it for ownership.
        "state.restore-props",      "false",
        "state.restore-target",     "false",
        nullptr);

    const bool virtual_dev = (e->kind == kEpVirtSink || e->kind == kEpVirtSource ||
                              e->kind == kEpCableSink);
    if (virtual_dev) pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    if (!e->target.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, e->target.c_str());
    // A bus feeding the stream sink must never be "helpfully" re-pointed at the
    // default device while the sink is missing: that plays the viewers' mix
    // into whoever is wearing the headphones.
    if (e->kind == kEpHwOut && e->target == kStreamSinkName)
        pw_properties_set(props, "node.dont-fallback", "true");

    e->stream = pw_stream_new(E->core, e->desc.c_str(), props);
    if (!e->stream) return false;
    pw_stream_add_listener(e->stream, &e->listener, &kStreamEvents, e);

    uint8_t buf[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    // Everything is stereo except a hardware bus in a surround mode, which asks
    // for the layout that mode publishes. The positions matter as much as the
    // count: without them PipeWire has no idea which speaker is which and puts
    // the whole thing through its own downmix.
    const BusLayout& L = bus_layout(e->kind == kEpHwOut ? e->mode : kBusNormal);
    info.channels = (uint32_t)L.channels;
    for (int c = 0; c < L.channels; ++c) info.position[c] = spa_position_for(L.chan[c]);
    const spa_pod* params[1] = { spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info) };

    uint32_t flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
    if (!virtual_dev) flags |= PW_STREAM_FLAG_AUTOCONNECT;   // hw ends follow a target

    const int r = pw_stream_connect(e->stream,
        is_input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
        PW_ID_ANY, (pw_stream_flags)flags, params, 1);
    if (r < 0) {
        std::fprintf(stderr, "[bb] connect %s failed: %s\n", e->desc.c_str(), spa_strerror(r));
        return false;
    }
    e->connected = true;
    return true;
}


// ---------------------------------------------------------------------------
// Tape deck. All file I/O happens on helper threads; the mixer only ever
// touches the lock-free rings.
// ---------------------------------------------------------------------------
static void read_rec_paths(Shared* shm, char rec_out[kNameLen], char play_out[kNameLen])
{
    for (int t = 0; t < 16; ++t) {
        const uint32_t s0 = shm->rec.cfg_seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        std::memcpy(rec_out,  shm->rec.rec_path,  kNameLen);
        std::memcpy(play_out, shm->rec.play_path, kNameLen);
        if (shm->rec.cfg_seq.load(std::memory_order_acquire) == s0) return;
    }
    rec_out[0] = play_out[0] = 0;
}

void Engine::start_record()
{
    if (rec_run.load()) return;
    char rp[kNameLen] = {}, pp[kNameLen] = {};
    read_rec_paths(shm, rp, pp);
    if (!rp[0]) { shm->rec.err.store(1); return; }

    SF_INFO info = {};
    info.samplerate = (int)sr;
    info.channels   = kChan;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    SNDFILE* f = sf_open(rp, SFM_WRITE, &info);
    if (!f) {
        std::fprintf(stderr, "[bb] record: cannot open %s: %s\n", rp, sf_strerror(nullptr));
        shm->rec.err.store(1);
        return;
    }

    rec_ring.clear();
    rec_dropped.store(0);
    shm->rec.frames_written.store(0);
    shm->rec.err.store(0);
    shm->rec.state.store(kRecRecording);
    rec_run.store(true);

    rec_thread = std::thread([this, f]() {
        std::vector<float> buf(4096 * kChan);
        auto drain = [&]() {
            uint32_t have;
            while ((have = rec_ring.avail()) > 0) {
                const uint32_t n = std::min<uint32_t>(have, 4096);
                rec_ring.read_padded(buf.data(), n);
                sf_writef_float(f, buf.data(), n);
                shm->rec.frames_written.fetch_add(n, std::memory_order_relaxed);
            }
        };
        while (rec_run.load(std::memory_order_relaxed)) {
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        drain();                       // flush whatever the mixer left behind
        sf_close(f);
    });
    std::fprintf(stderr, "[bb] recording bus %d -> %s\n", shm->rec.source_bus.load(), rp);
}

void Engine::stop_record()
{
    if (!rec_run.load()) return;
    rec_run.store(false);
    if (rec_thread.joinable()) rec_thread.join();
    shm->rec.state.store(kRecIdle);
    const uint32_t d = rec_dropped.load();
    std::fprintf(stderr, "[bb] recording stopped: %u frames%s\n",
                 shm->rec.frames_written.load(),
                 d ? " (WITH DROPOUTS - disk too slow)" : "");
}

void Engine::start_play()
{
    if (play_run.load()) return;
    char rp[kNameLen] = {}, pp[kNameLen] = {};
    read_rec_paths(shm, rp, pp);
    if (!pp[0]) { shm->rec.err.store(2); return; }

    SF_INFO info = {};
    SNDFILE* f = sf_open(pp, SFM_READ, &info);
    if (!f) {
        std::fprintf(stderr, "[bb] play: cannot open %s: %s\n", pp, sf_strerror(nullptr));
        shm->rec.err.store(2);
        return;
    }

    play_ring.clear();
    // Converted to the engine's rate, because frames_played counts frames the
    // resampler produced, and the GUI divides both by the same number. Stored
    // at the file's own rate, a three-minute 44.1 kHz take opened reading
    // "0:00 / 2:45" and then counted past its own total.
    shm->rec.total_frames.store(info.samplerate > 0
        ? (uint32_t)(info.frames * (sf_count_t)sr / info.samplerate)
        : (uint32_t)info.frames);
    shm->rec.frames_played.store(0);
    shm->rec.err.store(0);
    shm->rec.state.store(kRecPlaying);
    play_run.store(true);

    play_thread = std::thread([this, f, info]() {
        const int    fc    = info.channels > 0 ? info.channels : 1;
        const double ratio = double(info.samplerate) / double(sr);   // input per output frame
        std::vector<float> raw(4096 * fc);
        std::vector<float> in;            // stereo input frames, pending
        double frac = 0.0;
        bool   eof  = false;
        std::vector<float> out(1024 * kChan);

        auto refill = [&]() {
            if (eof) return;
            const sf_count_t got = sf_readf_float(f, raw.data(), 4096);
            if (got <= 0) {
                if (shm->rec.loop.load()) { sf_seek(f, 0, SEEK_SET); return; }
                eof = true;
                return;
            }
            for (sf_count_t i = 0; i < got; ++i) {
                float L, R;
                if (fc == 1)      { L = R = raw[i]; }
                else              { L = raw[i * fc]; R = raw[i * fc + 1]; }
                in.push_back(L); in.push_back(R);
            }
        };

        while (play_run.load(std::memory_order_relaxed)) {
            // Keep roughly a quarter of the ring queued ahead.
            while (play_ring.avail() < kRingFrames / 4) {
                // Need input frames covering [frac, frac+1].
                while (!eof && in.size() / kChan < size_t(frac) + 3) refill();
                const size_t base = size_t(frac);
                if (in.size() / kChan < base + 2) { if (eof) break; else continue; }

                uint32_t made = 0;
                while (made < 1024 && in.size() / kChan >= size_t(frac) + 2) {
                    const size_t i = size_t(frac);
                    const float  t = float(frac - double(i));
                    for (int c = 0; c < kChan; ++c) {
                        const float a = in[i * kChan + c], b = in[(i + 1) * kChan + c];
                        out[made * kChan + c] = a + (b - a) * t;
                    }
                    ++made;
                    frac += ratio;
                }
                if (!made) break;
                play_ring.write(out.data(), made);
                shm->rec.frames_played.fetch_add(made, std::memory_order_relaxed);

                // Drop fully-consumed input and rebase the fractional cursor.
                const size_t drop = size_t(frac);
                if (drop > 0) {
                    in.erase(in.begin(), in.begin() + drop * kChan);
                    frac -= double(drop);
                }
            }
            if (eof && in.size() / kChan < 2 && play_ring.avail() == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sf_close(f);
        // Reaching the end of the file stops the deck on its own.
        if (play_run.load()) {
            play_run.store(false);
            shm->rec.state.store(kRecIdle);
        }
    });
    std::fprintf(stderr, "[bb] playing %s (%lld frames @ %d Hz, %d ch)\n",
                 pp, (long long)info.frames, info.samplerate, info.channels);
}

void Engine::stop_play()
{
    if (!play_run.load() && !play_thread.joinable()) return;
    play_run.store(false);
    if (play_thread.joinable()) play_thread.join();
    play_ring.clear();
    shm->rec.state.store(kRecIdle);
}

// ---------------------------------------------------------------------------
// VBAN. Each enabled stream is one PipeWire module; senders additionally get a
// local playback endpoint that feeds the sink the module publishes.
// ---------------------------------------------------------------------------
void Engine::apply_vban()
{
    VbanOutCfg out[kVbanStreams];
    VbanInCfg  in [kVbanStreams];
    uint32_t seq = 0;
    bool ok = false;
    for (int t = 0; t < 16 && !ok; ++t) {
        const uint32_t s0 = shm->vban.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        std::memcpy(out, shm->vban.out, sizeof(out));
        std::memcpy(in,  shm->vban.in,  sizeof(in));
        if (shm->vban.seq.load(std::memory_order_acquire) == s0) { seq = s0; ok = true; }
    }
    if (!ok) return;
    vban_seen = seq;

    char buf[1024];

    for (int i = 0; i < kVbanStreams; ++i) {
        // ---- sender ----
        std::string want;
        if (out[i].enabled && out[i].host[0]) {
            std::snprintf(buf, sizeof(buf),
                "{ vban.destination.ip = \"%s\" vban.destination.port = %d sess.name = \"%s\" "
                "audio.rate = %d audio.channels = %d audio.format = \"S16LE\" "
                "stream.props = { media.class = \"Audio/Sink\" node.name = \"bb_vban_out_%d\" "
                "node.description = \"VBAN Out %d (%s)\" } }",
                out[i].host, out[i].port, out[i].name,
                out[i].rate, out[i].channels, i + 1, i + 1, out[i].name);
            want = buf;
        }
        if (want != vban_out_args[i]) {
            if (vban_ep[i].stream) { pw_stream_destroy(vban_ep[i].stream); vban_ep[i].stream = nullptr; }
            if (vban_out_mod[i])   { pw_impl_module_destroy(vban_out_mod[i]); vban_out_mod[i] = nullptr; }
            vban_out_args[i] = want;
            if (!want.empty()) {
                vban_out_mod[i] = pw_context_load_module(ctx, "libpipewire-module-vban-send",
                                                         want.c_str(), nullptr);
                if (!vban_out_mod[i]) {
                    std::fprintf(stderr, "[bb] VBAN out %d: module failed to load\n", i + 1);
                    vban_out_args[i].clear();
                } else {
                    Endpoint& e = vban_ep[i];
                    e.eng = this; e.kind = kEpVbanOut; e.index = i;
                    e.vban_bus = clampi(out[i].source_bus, 0, kBuses - 1);
                    e.node_name = "bb_vban_src_" + std::to_string(i + 1);
                    e.desc = "VBAN Send " + std::to_string(i + 1);
                    e.target = "bb_vban_out_" + std::to_string(i + 1);
                    vban_ring[i].clear();
                    connect_endpoint(this, &e);
                    std::fprintf(stderr, "[bb] VBAN out %d: bus %d -> %s:%d '%s'\n",
                                 i + 1, e.vban_bus, out[i].host, out[i].port, out[i].name);
                }
            }
        } else if (vban_ep[i].stream) {
            vban_ep[i].vban_bus = clampi(out[i].source_bus, 0, kBuses - 1);   // cheap to retarget
        }

        // ---- receiver ----
        std::string wantIn;
        if (in[i].enabled) {
            std::snprintf(buf, sizeof(buf),
                "{ vban.ip = \"0.0.0.0\" vban.port = %d sess.name = \"%s\" "
                "audio.rate = %d audio.channels = %d audio.format = \"S16LE\" "
                "stream.props = { media.class = \"Audio/Source\" node.name = \"bb_vban_in_%d\" "
                "node.description = \"VBAN In %d (%s)\" } }",
                in[i].port, in[i].name, in[i].rate, in[i].channels, i + 1, i + 1, in[i].name);
            wantIn = buf;
        }
        if (wantIn != vban_in_args[i]) {
            if (vban_in_mod[i]) { pw_impl_module_destroy(vban_in_mod[i]); vban_in_mod[i] = nullptr; }
            vban_in_args[i] = wantIn;
            if (!wantIn.empty()) {
                vban_in_mod[i] = pw_context_load_module(ctx, "libpipewire-module-vban-recv",
                                                        wantIn.c_str(), nullptr);
                if (!vban_in_mod[i]) {
                    std::fprintf(stderr, "[bb] VBAN in %d: module failed to load\n", i + 1);
                    vban_in_args[i].clear();
                } else {
                    std::fprintf(stderr, "[bb] VBAN in %d: port %d '%s' -> source bb_vban_in_%d\n",
                                 i + 1, in[i].port, in[i].name, i + 1);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Non-realtime control poll: device reassignment and commands.
// ---------------------------------------------------------------------------
void Engine::poll_control()
{
    char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
    char hwd[kHwStrips][kNameLen], outd[kPhysBuses][kNameLen];
    uint32_t hwg[kHwStrips] = {}, outg[kPhysBuses] = {};
    uint32_t seq = 0;
    if (routing_read(shm->routing, seq, hw, out, hwd, outd, hwg, outg) && seq != routing_seen) {
        routing_seen = seq;
        // Re-point anything whose node.name has moved since the preset was saved.
        for (int i = 0; i < kHwStrips; ++i) {
            const std::string r = resolve_device(this, hw[i], hwd[i], Direction::Capture);
            if (r != hw[i]) std::snprintf(hw[i], kNameLen, "%s", r.c_str());
        }
        for (int b = 0; b < kPhysBuses; ++b) {
            const std::string r = resolve_device(this, out[b], outd[b], Direction::Playback);
            if (r != out[b]) std::snprintf(out[b], kNameLen, "%s", r.c_str());
        }
        // Recompute cable assignment from scratch: a cable feeds at most one
        // strip, and a strip takes audio from at most one place.
        for (int c = 0; c < kCables; ++c) cable_target[c].store(-1, std::memory_order_relaxed);
        for (int i = 0; i < kHwStrips; ++i) {
            const std::string t = hw[i];
            const bool is_cable = t.rfind(kCablePrefix, 0) == 0;
            if (!is_cable) continue;
            const int c = atoi(t.c_str() + std::strlen(kCablePrefix));
            if (c >= 0 && c < kCables) cable_target[c].store(i, std::memory_order_relaxed);
        }

        for (int i = 0; i < kHwStrips; ++i) {
            std::string t = hw[i];
            // An explicit route asks for the link to be rebuilt even when the
            // name is the one already stored - see Routing::hw_in_gen.
            const bool asked = (hwg[i] != hw_gen_seen[i]);
            hw_gen_seen[i] = hwg[i];
            if (t != ep_in[i].target || asked) {
                ep_in[i].target = t;
                const bool is_cable = t.rfind(kCablePrefix, 0) == 0;
                std::fprintf(stderr, "[bb] HW IN %d -> %s\n", i + 1,
                             t.empty() ? "(none)" : t.c_str());
                // A cable feeds the strip directly, so no capture stream.
                if (ep_in[i].stream) { pw_stream_destroy(ep_in[i].stream); ep_in[i].stream = nullptr; }
                if (t.empty() || is_cable) {
                    shm->strip[i].present.store(is_cable ? 1 : 0, std::memory_order_relaxed);
                    if (!is_cable) strip_ring[i].clear();
                    shm->in_latency_ms[i].store(-1.0f, std::memory_order_relaxed);
                } else {
                    shm->in_latency_ms[i].store(-1.0f, std::memory_order_relaxed);
                    connect_endpoint(this, &ep_in[i]);
                    const NodeInfo* n = find_node(nodes, t);
                    if (n && n->desc != hwd[i]) {
                        routing_write_begin(shm->routing);
                        std::snprintf(shm->routing.hw_in_desc[i], kNameLen, "%s", n->desc.c_str());
                        std::snprintf(shm->routing.hw_in[i], kNameLen, "%s", t.c_str());
                        routing_write_end(shm->routing);
                        routing_seen = shm->routing.seq.load(std::memory_order_acquire);
                    }
                }
            }
        }
        for (int b = 0; b < kPhysBuses; ++b) {
            std::string t = out[b];
            const bool asked = (outg[b] != bus_gen_seen[b]);
            bus_gen_seen[b] = outg[b];
            if (t != ep_out[b].target || asked) {
                ep_out[b].target = t;
                std::fprintf(stderr, "[bb] BUS A%d -> %s\n", b + 1, t.empty() ? "(none)" : t.c_str());
                bus_waiting[b] = false;
                if (t.empty()) {
                    if (ep_out[b].stream) { pw_stream_destroy(ep_out[b].stream); ep_out[b].stream = nullptr; }
                    shm->out_latency_ms[b].store(-1.0f, std::memory_order_relaxed);
                } else if (t == kStreamSinkName && !find_node(nodes, t)) {
                    // Not there yet (the engine creates it): wait for it rather
                    // than connect and risk a fallback. stream_sink_tick wires
                    // the bus the moment the sink appears.
                    if (ep_out[b].stream) { pw_stream_destroy(ep_out[b].stream); ep_out[b].stream = nullptr; }
                    shm->out_latency_ms[b].store(-1.0f, std::memory_order_relaxed);
                    bus_waiting[b] = true;
                    std::fprintf(stderr, "[bb] BUS A%d waits for %s\n", b + 1, kStreamSinkName);
                } else {
                    // Re-read for the new device rather than carrying the old
                    // one's figure across; the two are rarely the same.
                    shm->out_latency_ms[b].store(-1.0f, std::memory_order_relaxed);
                    connect_endpoint(this, &ep_out[b]);
                    const NodeInfo* n = find_node(nodes, t);
                    if (n && n->desc != outd[b]) {
                        routing_write_begin(shm->routing);
                        std::snprintf(shm->routing.bus_out_desc[b], kNameLen, "%s", n->desc.c_str());
                        std::snprintf(shm->routing.bus_out[b], kNameLen, "%s", t.c_str());
                        routing_write_end(shm->routing);
                        routing_seen = shm->routing.seq.load(std::memory_order_acquire);
                    }
                }
            }
        }
    }

    // A bus mode changes how many channels the stream carries, so it cannot be
    // applied to a live stream - the node has to be republished with the new
    // layout. Only the A buses: B1 and B2 are what other applications record
    // from, and those are stereo by definition.
    for (int b = 0; b < kPhysBuses; ++b) {
        int want = shm->bus[b].mode.load(std::memory_order_relaxed);
        if (want < 0 || want >= kBusModeCount) want = kBusNormal;
        if (want == ep_out[b].mode) continue;
        ep_out[b].mode = want;
        ep_out[b].upmix.configure(sr);
        std::fprintf(stderr, "[bb] BUS A%d mode -> %s (%d ch)\n",
                     b + 1, bus_layout(want).name, bus_layout(want).channels);
        if (!ep_out[b].target.empty() && !bus_waiting[b]) connect_endpoint(this, &ep_out[b]);
    }

    stream_sink_tick();

    // The guard runs on graph events; also when what it was told changes (the
    // mode, the stream bus, where AUX goes), and every few seconds regardless.
    {
        int aux = 0;
        for (int b = 0; b < kPhysBuses; ++b)
            if (shm->strip[kAuxStrip].bus_on[b].load(std::memory_order_relaxed)) aux |= 1 << b;
        const int sig = shm->stream_guard_mode.load(std::memory_order_relaxed) * 1000
                      + (shm->stream.bus.load(std::memory_order_relaxed) + 1) * 10 + aux;
        const double now = now_s();
        if (sig != guard_sig || now >= guard_safety_at) {
            guard_sig = sig;
            guard_safety_at = now + 5.0;
            schedule_guard();
        }
    }

    // A click train left running is miserable, and the GUI that started it can
    // die, hang or be killed with the dialog open. Time it out here rather than
    // trusting the other end to clear it.
    {
        const int mask = shm->click_mask.load(std::memory_order_relaxed);
        const time_t now = time(nullptr);
        if (!mask) click_since = 0;
        else if (!click_since) click_since = now;
        else if (now - click_since >= kClickMaxSec) {
            shm->click_mask.store(0, std::memory_order_relaxed);
            click_since = 0;
            std::fprintf(stderr, "[bb] timing click stopped after %d s\n", kClickMaxSec);
        }
    }

    const uint32_t cs = shm->cmd_seq.load(std::memory_order_acquire);
    if (cs != cmd_seen) {
        cmd_seen = cs;
        switch (shm->cmd.load(std::memory_order_relaxed)) {
        case kCmdClearClip:
            for (int i = 0; i < kStrips; ++i) shm->meters.strip_clip[i].store(0);
            for (int b = 0; b < kBuses;  ++b) shm->meters.bus_clip[b].store(0);
            break;
        case kCmdResetLoudness:
            // The integrated figure measures a take, not a level, so it is the
            // one thing here with a "start again". Short-term keeps running.
            for (int b = 0; b < kBuses; ++b) bdsp[b].loud.reset_integrated();
            break;
        case kCmdResetMeters:
            for (int i = 0; i < kStrips; ++i)
                for (int c = 0; c < kChan; ++c) { sdsp[i].pre[c].reset(); sdsp[i].post[c].reset(); }
            for (int b = 0; b < kBuses; ++b)
                for (int c = 0; c < kChan; ++c) bdsp[b].meter[c].reset();
            break;
        case kCmdRecStart:  stop_play();   start_record(); break;
        case kCmdRecStop:   stop_record(); break;
        case kCmdPlayStart: stop_record(); start_play();   break;
        case kCmdPlayStop:  stop_play();   break;
        case kCmdVbanReload: apply_vban(); break;
        case kCmdQuit: g_run = 0; pw_main_loop_quit(loop); break;
        default: break;
        }
    }

    // The recorder thread clears play_run by itself when a file ends.
    if (!play_run.load() && play_thread.joinable() &&
        shm->rec.state.load() != kRecPlaying) {
        play_thread.join();
    }

    if (shm->vban.seq.load(std::memory_order_acquire) != vban_seen) apply_vban();
}

// ---------------------------------------------------------------------------
// Spectrum analysis. Runs on the control thread on its own faster timer, and
// does nothing at all while no editor is asking for a signal.
// ---------------------------------------------------------------------------
void Engine::poll_spectrum()
{
    const int src = shm->spec.source.load(std::memory_order_relaxed);
    if (src < 0 || src >= kSpecSourceCount) {
        if (shm->spec.active.load(std::memory_order_relaxed) != kSpecNone) {
            spec_an.reset();
            for (int k = 0; k < kSpecBins; ++k)
                shm->spec.bin_db[k].store(SpectrumAnalyzer::kFloorDb, std::memory_order_relaxed);
            shm->spec.active.store(kSpecNone, std::memory_order_relaxed);
            shm->spec.seq.fetch_add(1, std::memory_order_release);
        }
        return;
    }
    if (src != shm->spec.active.load(std::memory_order_relaxed)) {
        spec_an.reset();
        shm->spec.active.store(src, std::memory_order_relaxed);
    }

    spec_tap.snapshot(spec_win, kSpecFft);
    // 1.5 dB per 50 ms tick is 30 dB/s: quick enough to follow music, slow
    // enough to read.
    spec_an.analyze(spec_win, sr, shm->spec.f_lo.load(std::memory_order_relaxed),
                    shm->spec.f_hi.load(std::memory_order_relaxed), 1.5f);
    for (int k = 0; k < kSpecBins; ++k)
        shm->spec.bin_db[k].store(spec_an.disp[k], std::memory_order_relaxed);
    shm->spec.seq.fetch_add(1, std::memory_order_release);
}

// Keeps exactly one stream sink in the graph and the stream bus wired to it.
//
// The engine creates the sink itself unless a node of that name already
// exists - which is what an older install's 99-bb-stream.conf provides, so
// upgrading never produces two sinks with one name (target resolution between
// those is a coin toss). If one appears from elsewhere while we own ours, ours
// goes. If ours is removed or fails, it is recreated with backoff.
void Engine::stream_sink_tick()
{
    // Which A bus is the stream bus: the one whose output is the sink.
    int sbus = -1;
    for (int b = 0; b < kPhysBuses && sbus < 0; ++b)
        if (ep_out[b].target == kStreamSinkName) sbus = b;
    shm->stream.bus.store(sbus, std::memory_order_relaxed);

    if (!synced) return;
    const double now = now_s();

    if (sink_dead && sink_proxy) {
        spa_hook_remove(&sink_listener);
        pw_proxy_destroy(sink_proxy);
        sink_proxy = nullptr;
        sink_id = SPA_ID_INVALID;
        sink_retry_at = now + sink_backoff;
        sink_backoff = std::min(sink_backoff * 2.0, 30.0);
    }
    sink_dead = false;

    // The live sink node, and whether it is somebody else's.
    uint32_t live = SPA_ID_INVALID;
    bool external = false;
    for (const auto& kv : nodes) {
        if (kv.second.name != kStreamSinkName || kv.second.media_class != "Audio/Sink") continue;
        if (sink_proxy && kv.first == sink_id) { live = kv.first; continue; }
        // While ours is still being created its node can show up before we
        // learn its id; do not mistake it for a stranger and destroy it.
        if (sink_proxy && sink_id == SPA_ID_INVALID) continue;
        external = true;
        if (live == SPA_ID_INVALID) live = kv.first;
    }

    if (sink_proxy && external) {
        std::fprintf(stderr, "[bb] another %s appeared - removing ours\n", kStreamSinkName);
        spa_hook_remove(&sink_listener);
        pw_proxy_destroy(sink_proxy);
        sink_proxy = nullptr;
        sink_id = SPA_ID_INVALID;
    }

    if (external) {
        shm->stream.sink_state.store(kSinkExternal, std::memory_order_relaxed);
    } else if (sink_proxy) {
        shm->stream.sink_state.store(sink_id != SPA_ID_INVALID && nodes.count(sink_id)
                                         ? kSinkOwned : kSinkCreating,
                                     std::memory_order_relaxed);
    } else if (now >= sink_retry_at) {
        auto* p = pw_properties_new(
            PW_KEY_FACTORY_NAME,     "support.null-audio-sink",
            PW_KEY_NODE_NAME,        kStreamSinkName,
            PW_KEY_NODE_DESCRIPTION, "BetterBanana Stream Bus (capture only - do not select)",
            PW_KEY_MEDIA_CLASS,      "Audio/Sink",
            "audio.position",        "[ FL FR ]",
            // Tells the mixer this is a capture point, not an output to pick.
            "betterbanana.capture-only", "true",
            // Load-bearing: a suspended null sink takes the bus node with it.
            "session.suspend-timeout-seconds", "0",
            // Lives exactly as long as the engine's connection, even on SIGKILL.
            PW_KEY_OBJECT_LINGER,    "false",
            nullptr);
        sink_proxy = static_cast<pw_proxy*>(pw_core_create_object(
            core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &p->dict, 0));
        pw_properties_free(p);
        if (sink_proxy) {
            pw_proxy_add_listener(sink_proxy, &sink_listener, &kSinkProxyEvents, this);
            std::fprintf(stderr, "[bb] creating %s\n", kStreamSinkName);
            shm->stream.sink_state.store(kSinkCreating, std::memory_order_relaxed);
        } else {
            sink_retry_at = now + sink_backoff;
            sink_backoff = std::min(sink_backoff * 2.0, 30.0);
            shm->stream.sink_state.store(kSinkFailed, std::memory_order_relaxed);
        }
    } else {
        shm->stream.sink_state.store(kSinkAbsent, std::memory_order_relaxed);   // backing off
    }
    if (live != SPA_ID_INVALID && !external && sink_proxy) sink_backoff = 1.0;

    // Wire the stream bus to the sink: any bus that was waiting, and every bus
    // pointed at it whenever the sink node itself is a new one (recreated, or
    // ours replacing an old external one).
    // sink_seen is kept while the sink is gone, so its replacement counts as
    // new and the bus is rebuilt onto it rather than left on a dead target.
    if (live == SPA_ID_INVALID) return;
    const bool fresh = live != sink_seen;
    for (int b = 0; b < kPhysBuses; ++b) {
        if (ep_out[b].target != kStreamSinkName) continue;
        if (!bus_waiting[b] && !(fresh && sink_seen != SPA_ID_INVALID)) continue;
        bus_waiting[b] = false;
        std::fprintf(stderr, "[bb] BUS A%d -> %s (sink is up)\n", b + 1, kStreamSinkName);
        shm->out_latency_ms[b].store(-1.0f, std::memory_order_relaxed);
        connect_endpoint(this, &ep_out[b]);
    }
    sink_seen = live;
}

// ---------------------------------------------------------------------------
// Stream guard: apply plan_guard() (engine/streamguard.h) to the live graph.
// Runs on the main loop, debounced: any registry change arms a 100 ms one-shot,
// and a burst of changes is handled in one pass.
// ---------------------------------------------------------------------------
static void on_link_error(void* data, int /*seq*/, int res, const char* message)
{
    auto* pl = static_cast<Engine::PendingLink*>(data);
    std::fprintf(stderr, "[bb] guard: link failed: %s (%s)\n", message ? message : "error",
                 spa_strerror(res));
    pl->done = true;
}

// Deliberately no "bound" handler: the link only counts as made once its
// global is in the registry, and so in `links`. Clearing the pending entry any
// earlier opens exactly the gap a duplicate gets made in.
static const pw_proxy_events kLinkProxyEvents = {
    .version = PW_VERSION_PROXY_EVENTS,
    .error = on_link_error,
};

void Engine::schedule_guard()
{
    if (guard_armed || !guard_timer) return;
    guard_armed = true;
    timespec val{0, 100 * 1000 * 1000}, itv{0, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(loop), guard_timer, &val, &itv, false);
}

void Engine::guard_work()
{
    guard_armed = false;
    const double now = now_s();

    // Link proxies: linger keeps the link, so the proxy can go once the link
    // is in the registry (or it failed, or it is long overdue).
    for (auto it = pending_make.begin(); it != pending_make.end();) {
        PendingLink& pl = **it;
        if (pl.done || now > pl.expires) {
            spa_hook_remove(&pl.hook);
            pw_proxy_destroy(pl.proxy);
            it = pending_make.erase(it);
        } else ++it;
    }
    for (auto it = pending_drop.begin(); it != pending_drop.end();)
        it = now > it->second ? pending_drop.erase(it) : std::next(it);

    if (!synced) return;

    // A capture whose target has not arrived yet: give it a moment.
    for (const auto& [id, bc] : bound_caps)
        if (!capture_target.count(id) && now - bc->since < 1.0) { schedule_guard(); return; }

    Graph g;
    g.nodes = nodes;
    g.ports = ports;
    g.links = links;
    g.capture_target = capture_target;
    for (const auto& [lid, t] : pending_drop) g.links.erase(lid);
    // Links already asked for count as present; they are numbered where no
    // real link id reaches, so they can never be dropped by mistake.
    constexpr uint32_t kPendingBase = 0xF0000000u;
    uint32_t fake = kPendingBase;
    for (const auto& pl : pending_make) g.links[fake++] = { pl->out, pl->in };

    GuardInput in;
    in.mode = shm->stream_guard_mode.load(std::memory_order_relaxed);
    in.stream_bus = shm->stream.bus.load(std::memory_order_relaxed);
    for (int b = 0; b < kPhysBuses; ++b)
        in.aux_on[b] = shm->strip[kAuxStrip].bus_on[b].load(std::memory_order_relaxed) != 0;

    const GuardPlan plan = plan_guard(g, in);
    shm->stream.guard_state.store(plan.state, std::memory_order_relaxed);
    shm->stream.captures.store(plan.captures, std::memory_order_relaxed);
    if (plan.state != guard_logged_state) {
        static const char* const names[] = { "off", "idle", "echo protection only (no stream bus)",
            "stream bus -> Discord's own capture of it",
            "Discord is not capturing the stream bus (window share?) - feeding every capture",
            "stream bus assigned but its node is not up" };
        if (plan.state >= 0 && plan.state <= kGuardBusMissing)
            std::fprintf(stderr, "[bb] guard: %s\n", names[plan.state]);
        guard_logged_state = plan.state;
    }

    auto port_name = [&](uint32_t pid) {
        const auto pi = ports.find(pid);
        if (pi == ports.end()) return std::to_string(pid);
        const auto ni = nodes.find(pi->second.node);
        return (ni != nodes.end() ? ni->second.name : std::to_string(pi->second.node))
               + ":" + pi->second.channel;
    };

    bool deferred = false;
    for (const auto& [lid, why] : plan.drop) {
        if (lid >= kPendingBase) continue;
        const LinkInfo& l = g.links.at(lid);
        const auto po = ports.find(l.out_port), pi = ports.find(l.in_port);
        const auto key = std::make_pair(po != ports.end() ? po->second.node : 0u,
                                        pi != ports.end() ? pi->second.node : 0u);
        // At most one drop a second per pair: if something keeps putting a
        // link back, keep removing it - the echo matters more - but do not let
        // reacting to every event turn that into a busy loop.
        auto& h = drop_hist[key];
        while (!h.empty() && now - h.front() > 30.0) h.pop_front();
        if (!h.empty() && now - h.back() < 1.0) { deferred = true; continue; }
        h.push_back(now);
        if (h.size() == 10) {
            shm->stream.relink_fights.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "[bb] guard: %s keeps being linked back into Discord\n",
                         port_name(l.out_port).c_str());
        }
        pw_registry_destroy(registry, lid);
        pending_drop[lid] = now + 2.0;
        (why == kDropEcho ? shm->stream.echo_dropped : shm->stream.dup_dropped)
            .fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[bb] guard: dropped link %u (%s -> %s, %s)\n", lid,
                     port_name(l.out_port).c_str(), kCaptureName,
                     why == kDropEcho ? "carries AUX" : "not the stream bus");
    }

    for (const auto& [out, inp] : plan.make) {
        const auto po = ports.find(out), pi = ports.find(inp);
        if (po == ports.end() || pi == ports.end()) continue;
        const std::string on = std::to_string(po->second.node), op = std::to_string(out);
        const std::string inn = std::to_string(pi->second.node), ip = std::to_string(inp);
        auto* props = pw_properties_new(
            PW_KEY_LINK_OUTPUT_NODE, on.c_str(),  PW_KEY_LINK_OUTPUT_PORT, op.c_str(),
            PW_KEY_LINK_INPUT_NODE,  inn.c_str(), PW_KEY_LINK_INPUT_PORT,  ip.c_str(),
            // Outlives its proxy; it goes away with either end anyway.
            PW_KEY_OBJECT_LINGER, "true",
            nullptr);
        auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
            core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
        pw_properties_free(props);
        if (!proxy) continue;
        auto pl = std::make_unique<PendingLink>();
        pl->e = this; pl->proxy = proxy; pl->out = out; pl->in = inp;
        pl->expires = now + 2.0;
        pw_proxy_add_listener(proxy, &pl->hook, &kLinkProxyEvents, pl.get());
        pending_make.push_back(std::move(pl));
        shm->stream.links_made.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[bb] guard: attached %s -> %s port %u\n",
                     port_name(out).c_str(), kCaptureName, inp);
    }

    if (deferred || !pending_make.empty() || !pending_drop.empty()) schedule_guard();
}

static void on_guard_timer(void* data, uint64_t /*expirations*/)
{
    static_cast<Engine*>(data)->guard_work();
}

static void on_timer(void* data, uint64_t /*expirations*/)
{
    static_cast<Engine*>(data)->poll_control();
}

static void on_spec_timer(void* data, uint64_t /*expirations*/)
{
    static_cast<Engine*>(data)->poll_spectrum();
}

static void on_sig(int) { g_run = 0; if (g_eng.loop) pw_main_loop_quit(g_eng.loop); }

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    enable_ftz();

    // Refuse to start a second engine: two of them would publish duplicate
    // node names and silently fight over the graph.
    {
        int probe = shm_open(shm_name(), O_RDWR, 0600);
        if (probe >= 0) {
            void* pm = mmap(nullptr, sizeof(Shared), PROT_READ, MAP_SHARED, probe, 0);
            if (pm != MAP_FAILED) {
                const Shared* other = static_cast<const Shared*>(pm);
                if (other->magic.load() == kMagic) {
                    const pid_t pid = other->engine_pid.load();
                    if (pid > 0 && pid != getpid() && kill(pid, 0) == 0) {
                        std::fprintf(stderr,
                            "[bb] another engine is already running (pid %d).\n"
                            "      Stop it first:  bb-ctl quit   (or kill %d)\n", pid, pid);
                        munmap(pm, sizeof(Shared));
                        close(probe);
                        return 1;
                    }
                }
                munmap(pm, sizeof(Shared));
            }
            close(probe);
        }
    }

    // Shared memory. Deliberately NOT unlinked: reusing the same inode keeps a
    // running GUI's mapping valid across an engine restart. set_defaults()
    // reinitialises the contents, and struct_size guards against layout drift.
    g_eng.shm_fd = shm_open(shm_name(), O_CREAT | O_RDWR, 0600);
    if (g_eng.shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(g_eng.shm_fd, sizeof(Shared)) < 0) { perror("ftruncate"); return 1; }
    void* m = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, g_eng.shm_fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    g_eng.shm = new (m) Shared();
    set_defaults(g_eng.shm);
    g_eng.shm->engine_pid.store(getpid());

    // Presets are explicit: exactly the one named by the startup marker is
    // restored, and nothing is written back when the engine stops. Device
    // assignment lands via the routing seqlock and is applied by the first
    // control poll, once the endpoints exist.
    {
        std::string migrated;
        if (migrate_autosave(&migrated))
            std::fprintf(stderr,
                "[bb] the old automatic session save is now the preset \"%s\", "
                "and is what loads at startup\n", migrated.c_str());
        const std::string want = startup_preset_name();
        if (want.empty()) {
            std::fprintf(stderr, "[bb] no startup preset set "
                                 "(bb-ctl preset startup <name>)\n");
        } else {
            const std::string path = preset_path_for(want);
            if (load_preset(g_eng.shm, path.c_str()))
                std::fprintf(stderr, "[bb] loaded startup preset \"%s\"\n", want.c_str());
            else
                std::fprintf(stderr, "[bb] startup preset \"%s\" could not be read (%s)\n",
                             want.c_str(), path.c_str());
        }
    }

    g_eng.spec_an.configure();
    for (int i = 0; i < kStrips; ++i) {
        g_eng.sdsp[i].configure(g_eng.sr);
        g_eng.duck_gain[i].configure(g_eng.sr, 8.0f);
        g_eng.duck_gain[i].snap(1.0f);
        g_eng.ssend[i].configure(g_eng.sr);
    }
    for (int b = 0; b < kBuses; ++b) g_eng.bdsp[b].configure(g_eng.sr);
    g_eng.click.configure(g_eng.sr);
    for (int b = 0; b < kPhysBuses; ++b) {
        int m = g_eng.shm->bus[b].mode.load();
        g_eng.ep_out[b].mode = (m >= 0 && m < kBusModeCount) ? m : kBusNormal;
        g_eng.ep_out[b].upmix.configure(g_eng.sr);
    }

    pw_init(&argc, &argv);
    g_eng.loop = pw_main_loop_new(nullptr);
    g_eng.ctx  = pw_context_new(pw_main_loop_get_loop(g_eng.loop), nullptr, 0);
    g_eng.core = pw_context_connect(g_eng.ctx, nullptr, 0);
    if (!g_eng.core) { std::fprintf(stderr, "[bb] cannot connect to PipeWire\n"); return 1; }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    g_eng.registry = pw_core_get_registry(g_eng.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(g_eng.registry, &g_eng.registry_listener,
                             &kRegistryEvents, &g_eng);
    pw_core_add_listener(g_eng.core, &g_eng.core_listener, &kCoreEvents, &g_eng);
    g_eng.sync_seq = pw_core_sync(g_eng.core, PW_ID_CORE, 0);

    static const char* hw_desc[kHwStrips] = { "Hardware Input 1", "Hardware Input 2", "Hardware Input 3" };
    for (int i = 0; i < kHwStrips; ++i) {
        Endpoint& e = g_eng.ep_in[i];
        e.eng = &g_eng; e.kind = kEpHwIn; e.index = i;
        e.node_name = "bb_hw_in" + std::to_string(i + 1);
        e.desc = hw_desc[i];
        // Left unconnected until the GUI assigns a device.
    }
    static const char* vs_name[kVirtStrips] = { "bb_vaio", "bb_aux", "bb_vaio3" };
    static const char* vs_desc[kVirtStrips] = { "BetterBanana VAIO", "BetterBanana AUX", "BetterBanana VAIO3" };
    for (int i = 0; i < kVirtStrips; ++i) {
        Endpoint& e = g_eng.ep_in[kHwStrips + i];
        e.eng = &g_eng; e.kind = kEpVirtSink; e.index = kHwStrips + i;
        e.node_name = vs_name[i]; e.desc = vs_desc[i];
        if (!connect_endpoint(&g_eng, &e)) return 1;
    }
    for (int c = 0; c < kCables; ++c) {
        g_eng.cable_target[c].store(-1);
        Endpoint& e = g_eng.cable_ep[c];
        e.eng = &g_eng; e.kind = kEpCableSink; e.index = c;
        e.node_name = "bb_cable" + std::to_string(c + 1);
        e.desc = "BetterBanana Cable " + std::to_string(c + 1);
        if (!connect_endpoint(&g_eng, &e)) return 1;
    }

    static const char* a_desc[kPhysBuses] = { "BetterBanana A1", "BetterBanana A2", "BetterBanana A3" };
    for (int b = 0; b < kPhysBuses; ++b) {
        Endpoint& e = g_eng.ep_out[b];
        e.eng = &g_eng; e.kind = kEpHwOut; e.index = b;
        e.node_name = "bb_a" + std::to_string(b + 1);
        e.desc = a_desc[b];
    }
    static const char* vb_name[kVirtBuses] = { "bb_b1", "bb_b2" };
    static const char* vb_desc[kVirtBuses] = { "BetterBanana Out B1", "BetterBanana Out B2" };
    for (int b = 0; b < kVirtBuses; ++b) {
        Endpoint& e = g_eng.ep_out[kPhysBuses + b];
        e.eng = &g_eng; e.kind = kEpVirtSource; e.index = kPhysBuses + b;
        e.node_name = vb_name[b]; e.desc = vb_desc[b];
        if (!connect_endpoint(&g_eng, &e)) return 1;
    }

    g_eng.timer = pw_loop_add_timer(pw_main_loop_get_loop(g_eng.loop), on_timer, &g_eng);
    timespec val{0, 200 * 1000 * 1000}, itv{0, 200 * 1000 * 1000};
    pw_loop_update_timer(pw_main_loop_get_loop(g_eng.loop), g_eng.timer, &val, &itv, false);

    // The analyser needs a faster tick than device polling does, and costs
    // nothing while no editor is asking for a signal.
    g_eng.guard_timer = pw_loop_add_timer(pw_main_loop_get_loop(g_eng.loop), on_guard_timer, &g_eng);

    g_eng.spec_timer = pw_loop_add_timer(pw_main_loop_get_loop(g_eng.loop), on_spec_timer, &g_eng);
    timespec sval{0, 50 * 1000 * 1000}, sitv{0, 50 * 1000 * 1000};
    pw_loop_update_timer(pw_main_loop_get_loop(g_eng.loop), g_eng.spec_timer, &sval, &sitv, false);

    std::fprintf(stderr,
        "[bb] engine up: 3 virtual sinks (VAIO/AUX/VAIO3), 2 virtual sources (B1/B2),\n"
        "      3 hw inputs + 3 hw outputs idle until assigned. shm=%s\n", shm_name());

    pw_main_loop_run(g_eng.loop);

    g_eng.stop_record();
    g_eng.stop_play();
    for (int i = 0; i < kVbanStreams; ++i) {
        if (g_eng.vban_ep[i].stream) pw_stream_destroy(g_eng.vban_ep[i].stream);
        if (g_eng.vban_out_mod[i])   pw_impl_module_destroy(g_eng.vban_out_mod[i]);
        if (g_eng.vban_in_mod[i])    pw_impl_module_destroy(g_eng.vban_in_mod[i]);
    }
    for (int c = 0; c < kCables; ++c) if (g_eng.cable_ep[c].stream) pw_stream_destroy(g_eng.cable_ep[c].stream);
    for (int i = 0; i < kStrips; ++i) if (g_eng.ep_in[i].stream)  pw_stream_destroy(g_eng.ep_in[i].stream);
    for (int b = 0; b < kBuses;  ++b) if (g_eng.ep_out[b].stream) pw_stream_destroy(g_eng.ep_out[b].stream);
    if (g_eng.sink_proxy) {
        spa_hook_remove(&g_eng.sink_listener);
        pw_proxy_destroy(g_eng.sink_proxy);
    }
    for (auto& [id, bc] : g_eng.bound_caps) { spa_hook_remove(&bc->hook); pw_proxy_destroy(bc->proxy); }
    g_eng.bound_caps.clear();
    for (auto& pl : g_eng.pending_make) { spa_hook_remove(&pl->hook); pw_proxy_destroy(pl->proxy); }
    g_eng.pending_make.clear();
    if (g_eng.core) pw_core_disconnect(g_eng.core);
    if (g_eng.ctx)  pw_context_destroy(g_eng.ctx);
    pw_main_loop_destroy(g_eng.loop);
    pw_deinit();
    // Clear the pid so the next engine knows nobody owns this segment, but
    // leave the segment itself in place for any GUI still mapped to it.
    g_eng.shm->engine_pid.store(0);
    munmap(m, sizeof(Shared));
    std::fprintf(stderr, "[bb] engine down\n");
    return 0;
}
