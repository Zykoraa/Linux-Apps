// betterbanana - shared state between engine and GUI.
//
// A single POSIX shared-memory segment carries everything. Continuous
// parameters are plain atomics the GUI writes and the realtime thread reads
// (relaxed ordering is fine: a fader landing a cycle late is inaudible).
// Device assignments are strings, so they sit behind a seqlock and are polled
// off the realtime thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace bb {

constexpr uint32_t kMagic      = 0x42423031;   // 'BB01'
constexpr uint32_t kVersion    = 4;
constexpr const char* kShmName = "/betterbanana.state";

constexpr int kHwStrips   = 3;                 // Hardware Input 1..3
constexpr int kVirtStrips = 2;                 // BetterBanana VAIO, AUX
constexpr int kStrips     = kHwStrips + kVirtStrips;
constexpr int kPhysBuses  = 3;                 // A1 A2 A3
constexpr int kVirtBuses  = 2;                 // B1 B2
constexpr int kBuses      = kPhysBuses + kVirtBuses;
constexpr int kChan       = 2;
constexpr int kBusEqBands = 6;
constexpr int kNameLen    = 192;
constexpr int kLabelLen   = 28;                // user-supplied strip/bus names
constexpr int kVbanStreams = 8;                // Banana offers 8 in and 8 out
constexpr int kCables      = 3;                // standalone virtual cables

// A hardware strip's routing string is either empty, a PipeWire source
// node.name, or "cable:N" naming one of the engine's own virtual cables.
constexpr const char* kCablePrefix = "cable:";

using af = std::atomic<float>;
using ai = std::atomic<int32_t>;
using au = std::atomic<uint32_t>;

// Bus modes. The surround variants of the original need >2 channel buses;
// stereo-only modes are implemented, the rest are reserved.
enum BusMode : int32_t {
    kBusNormal = 0, kBusAmix, kBusBmix, kBusRepeat, kBusComposite,
    kBusTvMix,  kBusUpMix21, kBusUpMix41, kBusUpMix61,
    kBusCenterOnly, kBusLfeOnly, kBusRearOnly, kBusModeCount
};

struct StripParams {
    ai  present;                  // strip has a live input attached
    ai  bus_on[kBuses];           // A1 A2 A3 B1 B2 assign buttons
    af  gain_db;                  // -60 .. +12
    ai  mute, solo, mono;
    af  gate, comp, audibility;   // 0 .. 10 knobs
    af  eq_low, eq_mid, eq_high;  // -12 .. +12 dB
    af  pan_x, pan_y;             // -1 .. +1
    ai  mono_source;              // fold a mono capture across both channels
    af  limit_db;                 // output limiter ceiling
    ai  duck_key;                 // this strip's level drives the ducker
    af  duck_depth_db;            // how far this strip drops while ducking (<= 0)
};

struct BusParams {
    af  gain_db;
    ai  mute, mono, eq_on, sel;
    ai  mode;                     // BusMode
    af  eq_gain[kBusEqBands];
    af  eq_freq[kBusEqBands];
    af  eq_q[kBusEqBands];
};

struct Meters {
    af strip_pre [kStrips][kChan];   // pre-fader, post-input-gain
    af strip_post[kStrips][kChan];   // post-fader, what feeds the matrix
    af bus_out   [kBuses][kChan];
    af strip_gate_gain[kStrips];     // 1.0 = open
    af strip_comp_gr  [kStrips];     // dB of gain reduction (<= 0)
    af strip_duck_gr  [kStrips];     // dB removed by the ducker (<= 0)
    ai strip_clip     [kStrips];     // latched; cleared by the GUI
    ai bus_clip       [kBuses];
    af duck_env;                     // 0..1, how open the ducker currently is
};

// Seqlock: writer bumps to odd, writes, bumps to even. Reader retries while
// odd or while the count changed underneath it.
struct Routing {
    au  seq;
    char hw_in  [kHwStrips][kNameLen];   // source node.name feeding strips 0..2
    char bus_out[kPhysBuses][kNameLen];  // sink node.name fed by A1..A3
    // Human-readable names for the same devices. Node names encode the USB port,
    // so they change when hardware is replugged; the description usually does
    // not, and lets the engine re-find a device whose node.name has moved.
    char hw_in_desc  [kHwStrips][kNameLen];
    char bus_out_desc[kPhysBuses][kNameLen];
};

enum Command : int32_t {
    kCmdNone = 0, kCmdReconnect, kCmdResetMeters, kCmdQuit,
    kCmdRecStart, kCmdRecStop, kCmdPlayStart, kCmdPlayStop, kCmdVbanReload,
    kCmdClearClip
};

enum RecState : int32_t { kRecIdle = 0, kRecRecording, kRecPlaying };

// The tape deck: records one bus to WAV, and plays a WAV back into the matrix
// through its own bus-assign row, the way Banana's recorder strip does.
struct Recorder {
    ai   state;                   // RecState
    ai   source_bus;              // bus captured while recording
    ai   bus_on[kBuses];          // where playback is routed
    af   gain_db;                 // playback gain
    ai   loop;
    au   frames_written;
    au   frames_played;
    au   total_frames;            // length of the loaded file, 0 if none
    ai   err;                     // non-zero if the last operation failed
    au   cfg_seq;                 // seqlock guarding the two paths below
    char rec_path[kNameLen];
    char play_path[kNameLen];
};

// VBAN stream configuration. Strings live behind vban.seq, and the engine
// applies changes from its (non-realtime) control timer by loading and
// unloading the corresponding PipeWire modules.
struct VbanOutCfg {
    int32_t enabled;
    int32_t source_bus;           // which bus is transmitted
    int32_t port;
    int32_t channels;
    int32_t rate;
    char    name[32];             // VBAN stream name
    char    host[64];             // destination IP
};

struct VbanInCfg {
    int32_t enabled;
    int32_t port;
    int32_t channels;
    int32_t rate;
    char    name[32];
};

// Custom names for strips and buses. Empty means "use the built-in name".
// Guarded by a seqlock because these are strings.
struct Labels {
    au   seq;
    char strip[kStrips][kLabelLen];
    char bus  [kBuses][kLabelLen];
};

struct VbanConfig {
    au         seq;
    VbanOutCfg out[kVbanStreams];
    VbanInCfg  in [kVbanStreams];
};

struct Shared {
    au  magic, version;
    au  struct_size;              // sizeof(Shared); guards against layout drift
    ai  engine_pid;
    af  samplerate;
    au  quantum;
    au  xruns;
    au  engine_heartbeat;         // engine bumps every graph cycle

    StripParams strip[kStrips];
    BusParams   bus[kBuses];
    Meters      meters;
    Routing     routing;
    Recorder    rec;
    VbanConfig  vban;
    Labels      labels;

    // Sidechain ducker: strips flagged duck_key pull down every strip that has
    // a duck_depth, so music gets out of the way while you talk.
    af  duck_threshold_db;        // trigger level
    af  duck_attack_ms;
    af  duck_release_ms;
    ai  duck_enabled;

    au  cmd_seq;                  // GUI bumps after setting cmd
    ai  cmd;
};

// Returns nullptr-safe compatibility check for a mapped segment.
inline bool shm_compatible(const Shared* s)
{
    return s && s->magic.load() == kMagic
             && s->version.load() == kVersion
             && s->struct_size.load() == (uint32_t)sizeof(Shared);
}

// Reads the custom labels; returns false if the caller should retry.
inline bool labels_read(const Labels& l, char strip[kStrips][kLabelLen],
                        char bus[kBuses][kLabelLen])
{
    const uint32_t s0 = l.seq.load(std::memory_order_acquire);
    if (s0 & 1u) return false;
    std::memcpy(strip, l.strip, sizeof(char) * kStrips * kLabelLen);
    std::memcpy(bus,   l.bus,   sizeof(char) * kBuses  * kLabelLen);
    return l.seq.load(std::memory_order_acquire) == s0;
}

inline void routing_write_begin(Routing& r) { r.seq.fetch_add(1, std::memory_order_acq_rel); }
inline void routing_write_end  (Routing& r) { r.seq.fetch_add(1, std::memory_order_release); }

// Returns false if the caller should retry.
inline bool routing_read(const Routing& r, uint32_t& seen,
                         char hw[kHwStrips][kNameLen],
                         char out[kPhysBuses][kNameLen],
                         char hwd[kHwStrips][kNameLen] = nullptr,
                         char outd[kPhysBuses][kNameLen] = nullptr)
{
    const uint32_t s0 = r.seq.load(std::memory_order_acquire);
    if (s0 & 1u) return false;
    std::memcpy(hw,  r.hw_in,   sizeof(char) * kHwStrips  * kNameLen);
    std::memcpy(out, r.bus_out, sizeof(char) * kPhysBuses * kNameLen);
    if (hwd)  std::memcpy(hwd,  r.hw_in_desc,   sizeof(char) * kHwStrips  * kNameLen);
    if (outd) std::memcpy(outd, r.bus_out_desc, sizeof(char) * kPhysBuses * kNameLen);
    const uint32_t s1 = r.seq.load(std::memory_order_acquire);
    if (s0 != s1) return false;
    seen = s1;
    return true;
}

inline void set_defaults(Shared* s)
{
    s->magic.store(kMagic);
    s->version.store(kVersion);
    s->struct_size.store((uint32_t)sizeof(Shared));
    s->samplerate.store(48000.0f);
    s->quantum.store(1024);
    s->xruns.store(0);

    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = s->strip[i];
        p.present.store(0);
        // Hardware strips default to A1; virtual strips to A1 + B1, which is
        // the layout most people end up building by hand anyway.
        for (int b = 0; b < kBuses; ++b) p.bus_on[b].store(0);
        p.bus_on[0].store(1);
        if (i >= kHwStrips) p.bus_on[kPhysBuses].store(1);
        p.gain_db.store(0.0f);
        p.mute.store(0); p.solo.store(0); p.mono.store(0);
        p.gate.store(0.0f); p.comp.store(0.0f); p.audibility.store(0.0f);
        p.eq_low.store(0.0f); p.eq_mid.store(0.0f); p.eq_high.store(0.0f);
        p.pan_x.store(0.0f); p.pan_y.store(0.0f);
        p.mono_source.store(0);
        p.limit_db.store(12.0f);
        p.duck_key.store(0);
        p.duck_depth_db.store(0.0f);
    }
    for (int b = 0; b < kBuses; ++b) {
        BusParams& p = s->bus[b];
        p.gain_db.store(0.0f);
        p.mute.store(0); p.mono.store(0); p.eq_on.store(0); p.sel.store(b == 0 ? 1 : 0);
        p.mode.store(kBusNormal);
        static const float f[kBusEqBands] = { 60, 160, 400, 1000, 3000, 8000 };
        for (int k = 0; k < kBusEqBands; ++k) {
            p.eq_gain[k].store(0.0f);
            p.eq_freq[k].store(f[k]);
            p.eq_q[k].store(1.0f);
        }
    }
    routing_write_begin(s->routing);
    std::memset(s->routing.hw_in, 0, sizeof(s->routing.hw_in));
    std::memset(s->routing.bus_out, 0, sizeof(s->routing.bus_out));
    std::memset(s->routing.hw_in_desc, 0, sizeof(s->routing.hw_in_desc));
    std::memset(s->routing.bus_out_desc, 0, sizeof(s->routing.bus_out_desc));
    routing_write_end(s->routing);

    s->rec.state.store(kRecIdle);
    s->rec.source_bus.store(0);
    for (int b = 0; b < kBuses; ++b) s->rec.bus_on[b].store(b == 0 ? 1 : 0);
    s->rec.gain_db.store(0.0f);
    s->rec.loop.store(0);
    s->rec.frames_written.store(0);
    s->rec.frames_played.store(0);
    s->rec.total_frames.store(0);
    s->rec.err.store(0);
    s->rec.cfg_seq.store(0);
    std::memset(s->rec.rec_path, 0, sizeof(s->rec.rec_path));
    std::memset(s->rec.play_path, 0, sizeof(s->rec.play_path));

    s->duck_enabled.store(0);
    s->duck_threshold_db.store(-34.0f);
    s->duck_attack_ms.store(12.0f);
    s->duck_release_ms.store(320.0f);
    for (int i = 0; i < kStrips; ++i) {
        s->meters.strip_duck_gr[i].store(0.0f);
        s->meters.strip_clip[i].store(0);
    }
    for (int b = 0; b < kBuses; ++b) s->meters.bus_clip[b].store(0);
    s->meters.duck_env.store(0.0f);

    s->labels.seq.store(0);
    std::memset(s->labels.strip, 0, sizeof(s->labels.strip));
    std::memset(s->labels.bus,   0, sizeof(s->labels.bus));

    s->vban.seq.store(0);
    for (int i = 0; i < kVbanStreams; ++i) {
        VbanOutCfg& o = s->vban.out[i];
        o.enabled = 0; o.source_bus = kPhysBuses; o.port = 6980 + i;
        o.channels = 2; o.rate = 48000;
        std::snprintf(o.name, sizeof(o.name), "Stream%d", i + 1);
        o.host[0] = 0;
        VbanInCfg& n = s->vban.in[i];
        n.enabled = 0; n.port = 6980 + i; n.channels = 2; n.rate = 48000;
        std::snprintf(n.name, sizeof(n.name), "Stream%d", i + 1);
    }
    s->cmd.store(kCmdNone);
    s->cmd_seq.store(0);
}

} // namespace bb
