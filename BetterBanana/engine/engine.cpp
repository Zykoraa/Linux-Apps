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

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <sndfile.h>
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
#include <string>
#include <map>
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
    SmoothGain gain[kChan];
    PeakMeter  pre[kChan], post[kChan];
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
    }
};

struct BusDsp {
    EqChain    eq;
    SmoothGain gain[kChan];
    PeakMeter  meter[kChan];

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) {
            gain[c].configure(sr, 15.0f); gain[c].snap(1.0f);
            meter[c].configure(sr);
        }
        eq.configure(sr);
    }
    void update(const BusParams& p, float sr) { eq.update(p.eq, sr); }
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
};

struct Engine {
    pw_main_loop* loop = nullptr;
    pw_context*   ctx  = nullptr;
    pw_core*      core = nullptr;
    pw_registry*  registry = nullptr;
    spa_hook      registry_listener = {};
    spa_source*   timer = nullptr;

    // node.name -> node.description for every live node. Node names encode the
    // USB port, so they move when hardware is replugged; the description lets
    // a saved preset find the device again.
    std::map<std::string, std::string> node_desc;
    std::map<uint32_t, std::string>    node_by_id;

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

    // scratch
    float stripout[kStrips][kMaxChunk * kChan];   // per-strip post-DSP output
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
};

static Engine g_eng;
static volatile sig_atomic_t g_run = 1;

static void on_registry_global(void* data, uint32_t id, uint32_t /*permissions*/,
                               const char* type, uint32_t /*version*/,
                               const spa_dict* props)
{
    if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name) return;
    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    Engine* e = static_cast<Engine*>(data);
    e->node_desc[name] = desc ? desc : "";
    e->node_by_id[id] = name;
}

static void on_registry_global_remove(void* data, uint32_t id)
{
    Engine* e = static_cast<Engine*>(data);
    auto it = e->node_by_id.find(id);
    if (it == e->node_by_id.end()) return;
    e->node_desc.erase(it->second);
    e->node_by_id.erase(it);
}

static const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

// If the saved node.name is gone but we know what the device was called, look
// for a live node advertising the same description.
static std::string resolve_device(Engine* e, const std::string& name, const char* desc)
{
    if (name.empty()) return name;
    if (name.rfind(kCablePrefix, 0) == 0) return name;      // virtual cable
    if (e->node_desc.count(name)) return name;              // still present
    if (!desc || !*desc) return name;
    for (const auto& kv : e->node_desc)
        if (kv.second == desc) {
            std::fprintf(stderr, "[bb] '%s' is gone; matched '%s' by description \"%s\"\n",
                         name.c_str(), kv.first.c_str(), desc);
            return kv.first;
        }
    return name;
}

// ---------------------------------------------------------------------------
// The mixer. Consumes n frames from every strip ring, produces n frames into
// every bus ring. Runs on the data-loop thread only.
// ---------------------------------------------------------------------------
void Engine::mix_chunk(uint32_t n)
{
    if (n > kMaxChunk) n = kMaxChunk;
    Shared* s = shm;

    for (int b = 0; b < kBuses; ++b)
        std::memset(acc[b], 0, sizeof(float) * n * kChan);

    // ---- pass 1: run each strip's DSP into its own buffer -----------------
    // Keeping the per-strip result lets the ducker see every key strip before
    // anything is summed, and lets solo be decided per bus.
    float key_peak = 0.0f;
    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        StripDsp& d = sdsp[i];

        strip_ring[i].drop_to(kResyncQuanta * n);
        strip_ring[i].read_padded(sbuf, n);
        d.update(p, sr);

        const bool mono_src = p.mono_source.load(std::memory_order_relaxed) != 0;
        const bool mono     = p.mono.load(std::memory_order_relaxed) != 0;
        const bool muted    = p.mute.load(std::memory_order_relaxed) != 0;
        const float glin    = db_to_lin(p.gain_db.load(std::memory_order_relaxed));
        float pl, pr; pan_gains(p.pan_x.load(std::memory_order_relaxed), pl, pr);

        const bool par_on   = p.eq.on.load(std::memory_order_relaxed) != 0;
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
                x *= d.gain[c].next();
                x *= (c == 0 ? pl : pr) * 1.41421356f;   // pan law is unity at centre

                const float b2 = std::fabs(x);
                if (b2 > post_pk[c]) post_pk[c] = b2;
                ch[c] = x;
            }
            stripout[i][f * kChan]     = ch[0];
            stripout[i][f * kChan + 1] = ch[1];
        }

        // A key strip drives the ducker from its post-processing level, so the
        // gate and compressor decide what counts as speech.
        if (p.duck_key.load(std::memory_order_relaxed))
            key_peak = std::max(key_peak, std::max(post_pk[0], post_pk[1]));

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
    // buses it actually feeds.
    bool solo_on_bus[kBuses] = {};
    for (int b = 0; b < kBuses; ++b)
        for (int i = 0; i < kStrips; ++i)
            if (s->strip[i].solo.load(std::memory_order_relaxed) &&
                s->strip[i].bus_on[b].load(std::memory_order_relaxed)) {
                solo_on_bus[b] = true;
                break;
            }

    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        const float depth = p.duck_depth_db.load(std::memory_order_relaxed);
        const float duck_db = depth * duck_env;
        s->meters.strip_duck_gr[i].store(duck_db, std::memory_order_relaxed);
        duck_gain[i].set_target(db_to_lin(duck_db));

        const bool soloed = p.solo.load(std::memory_order_relaxed) != 0;
        for (uint32_t f = 0; f < n; ++f) {
            const float dg = duck_gain[i].next();
            const float L = stripout[i][f * kChan]     * dg;
            const float R = stripout[i][f * kChan + 1] * dg;
            for (int b = 0; b < kBuses; ++b) {
                if (!p.bus_on[b].load(std::memory_order_relaxed)) continue;
                if (solo_on_bus[b] && !soloed) continue;
                acc[b][f * kChan]     += L;
                acc[b][f * kChan + 1] += R;
            }
        }
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

    // Bus stage: EQ -> mono -> gain -> limiter.
    for (int b = 0; b < kBuses; ++b) {
        BusParams& p = s->bus[b];
        BusDsp& d = bdsp[b];
        d.update(p, sr);

        const bool eq_on = p.eq.on.load(std::memory_order_relaxed) != 0;
        const bool mono  = p.mono.load(std::memory_order_relaxed) != 0;
        const bool muted = p.mute.load(std::memory_order_relaxed) != 0;
        const float glin = db_to_lin(p.gain_db.load(std::memory_order_relaxed));
        for (int c = 0; c < kChan; ++c) d.gain[c].set_target(muted ? 0.0f : glin);

        float pk[kChan] = {0, 0};
        for (uint32_t f = 0; f < n; ++f) {
            float L = acc[b][f * kChan], R = acc[b][f * kChan + 1];
            if (mono) { const float m = 0.5f * (L + R); L = R = m; }
            float ch[kChan] = { L, R };
            for (int c = 0; c < kChan; ++c) {
                float x = ch[c];
                if (eq_on) x = d.eq.process(c, x);
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
        bus_ring[b].drop_to(kResyncQuanta * n);
        bus_ring[b].write(acc[b], n);
    }

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
            if (nc == kChan) {
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

static void on_param_changed(void* data, uint32_t id, const spa_pod* param)
{
    if (!param || id != SPA_PARAM_Format) return;
    Endpoint* e = static_cast<Endpoint*>(data);
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
        nullptr);

    const bool virtual_dev = (e->kind == kEpVirtSink || e->kind == kEpVirtSource ||
                              e->kind == kEpCableSink);
    if (virtual_dev) pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    if (!e->target.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, e->target.c_str());

    e->stream = pw_stream_new(E->core, e->desc.c_str(), props);
    if (!e->stream) return false;
    pw_stream_add_listener(e->stream, &e->listener, &kStreamEvents, e);

    uint8_t buf[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChan;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
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
    shm->rec.total_frames.store((uint32_t)info.frames);
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
    uint32_t seq = 0;
    if (routing_read(shm->routing, seq, hw, out, hwd, outd) && seq != routing_seen) {
        routing_seen = seq;
        // Re-point anything whose node.name has moved since the preset was saved.
        for (int i = 0; i < kHwStrips; ++i) {
            const std::string r = resolve_device(this, hw[i], hwd[i]);
            if (r != hw[i]) std::snprintf(hw[i], kNameLen, "%s", r.c_str());
        }
        for (int b = 0; b < kPhysBuses; ++b) {
            const std::string r = resolve_device(this, out[b], outd[b]);
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
            if (t != ep_in[i].target) {
                ep_in[i].target = t;
                const bool is_cable = t.rfind(kCablePrefix, 0) == 0;
                std::fprintf(stderr, "[bb] HW IN %d -> %s\n", i + 1,
                             t.empty() ? "(none)" : t.c_str());
                // A cable feeds the strip directly, so no capture stream.
                if (ep_in[i].stream) { pw_stream_destroy(ep_in[i].stream); ep_in[i].stream = nullptr; }
                if (t.empty() || is_cable) {
                    shm->strip[i].present.store(is_cable ? 1 : 0, std::memory_order_relaxed);
                    if (!is_cable) strip_ring[i].clear();
                } else {
                    connect_endpoint(this, &ep_in[i]);
                    auto it = node_desc.find(t);
                    if (it != node_desc.end() && it->second != hwd[i]) {
                        routing_write_begin(shm->routing);
                        std::snprintf(shm->routing.hw_in_desc[i], kNameLen, "%s", it->second.c_str());
                        std::snprintf(shm->routing.hw_in[i], kNameLen, "%s", t.c_str());
                        routing_write_end(shm->routing);
                        routing_seen = shm->routing.seq.load(std::memory_order_acquire);
                    }
                }
            }
        }
        for (int b = 0; b < kPhysBuses; ++b) {
            std::string t = out[b];
            if (t != ep_out[b].target) {
                ep_out[b].target = t;
                std::fprintf(stderr, "[bb] BUS A%d -> %s\n", b + 1, t.empty() ? "(none)" : t.c_str());
                if (t.empty()) {
                    if (ep_out[b].stream) { pw_stream_destroy(ep_out[b].stream); ep_out[b].stream = nullptr; }
                } else {
                    connect_endpoint(this, &ep_out[b]);
                    auto it = node_desc.find(t);
                    if (it != node_desc.end() && it->second != outd[b]) {
                        routing_write_begin(shm->routing);
                        std::snprintf(shm->routing.bus_out_desc[b], kNameLen, "%s", it->second.c_str());
                        std::snprintf(shm->routing.bus_out[b], kNameLen, "%s", t.c_str());
                        routing_write_end(shm->routing);
                        routing_seen = shm->routing.seq.load(std::memory_order_acquire);
                    }
                }
            }
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
        int probe = shm_open(kShmName, O_RDWR, 0600);
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
    g_eng.shm_fd = shm_open(kShmName, O_CREAT | O_RDWR, 0600);
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
    }
    for (int b = 0; b < kBuses; ++b) g_eng.bdsp[b].configure(g_eng.sr);

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

    static const char* hw_desc[kHwStrips] = { "Hardware Input 1", "Hardware Input 2", "Hardware Input 3" };
    for (int i = 0; i < kHwStrips; ++i) {
        Endpoint& e = g_eng.ep_in[i];
        e.eng = &g_eng; e.kind = kEpHwIn; e.index = i;
        e.node_name = "bb_hw_in" + std::to_string(i + 1);
        e.desc = hw_desc[i];
        // Left unconnected until the GUI assigns a device.
    }
    static const char* vs_name[kVirtStrips] = { "bb_vaio", "bb_aux" };
    static const char* vs_desc[kVirtStrips] = { "BetterBanana VAIO", "BetterBanana AUX" };
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
    g_eng.spec_timer = pw_loop_add_timer(pw_main_loop_get_loop(g_eng.loop), on_spec_timer, &g_eng);
    timespec sval{0, 50 * 1000 * 1000}, sitv{0, 50 * 1000 * 1000};
    pw_loop_update_timer(pw_main_loop_get_loop(g_eng.loop), g_eng.spec_timer, &sval, &sitv, false);

    std::fprintf(stderr,
        "[bb] engine up: 2 virtual sinks (VAIO/AUX), 2 virtual sources (B1/B2),\n"
        "      3 hw inputs + 3 hw outputs idle until assigned. shm=%s\n", kShmName);

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
