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
constexpr uint32_t kVersion    = 8;
constexpr const char* kShmName = "/betterbanana.state";

constexpr int kHwStrips   = 3;                 // Hardware Input 1..3
constexpr int kVirtStrips = 2;                 // BetterBanana VAIO, AUX
constexpr int kStrips     = kHwStrips + kVirtStrips;
constexpr int kPhysBuses  = 3;                 // A1 A2 A3
constexpr int kVirtBuses  = 2;                 // B1 B2
constexpr int kBuses      = kPhysBuses + kVirtBuses;
constexpr int kChan       = 2;
constexpr int kEqBands    = 12;
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

// Filter shapes an EQ band can take. The names match the Equalizer APO /
// Peace / AutoEq vocabulary so an imported profile maps across one-to-one:
// PK, LSC/LS, HSC/HS, LPQ/LP, HPQ/HP, NO, BP.
enum EqFilterType : int32_t {
    kEqPeak = 0, kEqLowShelf, kEqHighShelf, kEqHighPass, kEqLowPass,
    kEqNotch, kEqBandPass, kEqTypeCount
};

// Bus modes. The surround variants of the original need >2 channel buses;
// stereo-only modes are implemented, the rest are reserved.
enum BusMode : int32_t {
    kBusNormal = 0, kBusAmix, kBusBmix, kBusRepeat, kBusComposite,
    kBusTvMix,  kBusUpMix21, kBusUpMix41, kBusUpMix61,
    kBusCenterOnly, kBusLfeOnly, kBusRearOnly, kBusModeCount
};

// A twelve-band parametric EQ. Buses have had one since v5; input strips gained
// the same block in v6, so one editor, one profile format and one engine chain
// serve both. `on` bypasses the whole block; `band_on` bypasses one band.
struct EqParams {
    ai  on;
    af  preamp_db;                // applied before the band chain
    af  gain[kEqBands];
    af  freq[kEqBands];
    af  q[kEqBands];
    ai  type[kEqBands];           // EqFilterType
    ai  band_on[kEqBands];
};

// Twelve bands spread over the audible range. Every band starts as a flat
// peaking filter, which the engine bypasses, so a fresh block sounds exactly
// like no EQ at all.
constexpr float kEqDefaultFreq[kEqBands] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000
};

inline void eq_set_defaults(EqParams& p)
{
    p.on.store(0);
    p.preamp_db.store(0.0f);
    for (int k = 0; k < kEqBands; ++k) {
        p.gain[k].store(0.0f);
        p.freq[k].store(kEqDefaultFreq[k]);
        p.q[k].store(1.0f);
        p.type[k].store(kEqPeak);
        p.band_on[k].store(1);
    }
}

// A voice changer: a pitch shifter followed by a small rack of character
// effects, per input strip. Every field has an "off" value (0, or 1 for the
// decimation factor) so a default block is bit-transparent and the engine
// skips it entirely.
//
// Chain order is pitch -> drive -> ring -> crush -> chorus -> echo -> gain:
// the structural change first, then character, then space, so the echo
// repeats the finished voice rather than the raw one.
struct VoiceFx {
    ai  on;                       // whole-block bypass
    af  pitch;                    // semitones, -12 .. +12; 0 bypasses
    // Formants are what carry perceived body size. The pitch shifter is a
    // resampler, so it drags them along with it - which is why a big shift
    // sounds like a small person. Turn this on and they are controlled
    // separately: `formant` is the NET shift, whatever pitch is doing.
    ai  formant_on;
    af  formant;                  // semitones, -12 .. +12
    af  drive;                    // 0 .. 10 waveshaper amount
    af  ring_hz;                  // ring modulator, 0 = off
    af  ring_mix;                 // 0 .. 1
    ai  bits;                     // bit-crush depth, 0 = off, else 2 .. 15
    ai  downsample;               // sample-and-hold factor, 1 = off
    af  echo_ms;                  // 0 = off, else up to 1000
    af  echo_fb;                  // 0 .. 0.95
    af  echo_mix;                 // 0 .. 1
    af  chorus_ms;                // modulation depth, 0 = off
    af  chorus_hz;                // 0.05 .. 8
    af  chorus_mix;               // 0 .. 1
    af  gain_db;                  // makeup, -24 .. +24
};

inline void fx_set_defaults(VoiceFx& p)
{
    p.on.store(0);
    p.pitch.store(0.0f);
    p.formant_on.store(0);
    p.formant.store(0.0f);
    p.drive.store(0.0f);
    p.ring_hz.store(0.0f);
    p.ring_mix.store(0.0f);
    p.bits.store(0);
    p.downsample.store(1);
    p.echo_ms.store(0.0f);
    p.echo_fb.store(0.0f);
    p.echo_mix.store(0.0f);
    p.chorus_ms.store(0.0f);
    p.chorus_hz.store(0.0f);
    p.chorus_mix.store(0.0f);
    p.gain_db.store(0.0f);
}

struct StripParams {
    ai  present;                  // strip has a live input attached
    ai  bus_on[kBuses];           // A1 A2 A3 B1 B2 assign buttons
    af  gain_db;                  // -60 .. +12
    ai  mute, solo, mono;
    af  gate, comp, audibility;   // 0 .. 10 knobs
    af  eq_low, eq_mid, eq_high;  // -12 .. +12 dB, the three fixed tone knobs
    af  pan_x, pan_y;             // -1 .. +1
    ai  mono_source;              // fold a mono capture across both channels
    af  limit_db;                 // output limiter ceiling
    ai  duck_key;                 // this strip's level drives the ducker
    af  duck_depth_db;            // how far this strip drops while ducking (<= 0)
    EqParams eq;                  // the parametric block, after the tone knobs
    VoiceFx  fx;                  // the voice changer, after the EQ
};

struct BusParams {
    af  gain_db;
    ai  mute, mono, sel;
    ai  mode;                     // BusMode
    EqParams eq;
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

// ---------------------------------------------------------------------------
// Spectrum analyser. One signal at a time: whichever EQ editor is open asks for
// its own, and nothing is measured while none is. Analysis runs on the engine's
// control thread, off the realtime path.
// ---------------------------------------------------------------------------
constexpr int kSpecBins = 64;
constexpr int kSpecNone = -1;

// Source encoding: buses occupy 0..kBuses-1, strips follow.
inline constexpr int spec_bus_src(int b)   { return b; }
inline constexpr int spec_strip_src(int i) { return kBuses + i; }
constexpr int kSpecSourceCount = kBuses + kStrips;

struct Spectrum {
    ai  source;                   // what a GUI wants measured; kSpecNone = idle
    ai  active;                   // what the engine is actually measuring
    au  seq;                      // bumped after every refresh
    af  f_lo, f_hi;               // frequency range the bins span
    af  bin_db[kSpecBins];        // dBFS, log-spaced, already peak-decayed
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
    // How much of its realtime deadline the mixer is actually using, in tenths
    // of a percent, as a decaying peak. This used to be an `xruns` counter that
    // nothing ever incremented - a gauge permanently reading zero is worse than
    // no gauge, and now that a strip can switch on an FFT this is the number
    // that says whether there is room for it.
    au  dsp_load;
    au  engine_heartbeat;         // engine bumps every graph cycle

    StripParams strip[kStrips];
    BusParams   bus[kBuses];
    Meters      meters;
    Spectrum    spec;
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
    s->dsp_load.store(0);

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
        eq_set_defaults(p.eq);
        fx_set_defaults(p.fx);
    }
    for (int b = 0; b < kBuses; ++b) {
        BusParams& p = s->bus[b];
        p.gain_db.store(0.0f);
        p.mute.store(0); p.mono.store(0); p.sel.store(b == 0 ? 1 : 0);
        p.mode.store(kBusNormal);
        eq_set_defaults(p.eq);
    }
    routing_write_begin(s->routing);
    std::memset(s->routing.hw_in, 0, sizeof(s->routing.hw_in));
    std::memset(s->routing.bus_out, 0, sizeof(s->routing.bus_out));
    std::memset(s->routing.hw_in_desc, 0, sizeof(s->routing.hw_in_desc));
    std::memset(s->routing.bus_out_desc, 0, sizeof(s->routing.bus_out_desc));
    routing_write_end(s->routing);

    s->spec.source.store(kSpecNone);
    s->spec.active.store(kSpecNone);
    s->spec.seq.store(0);
    s->spec.f_lo.store(20.0f);
    s->spec.f_hi.store(20000.0f);
    for (int k = 0; k < kSpecBins; ++k) s->spec.bin_db[k].store(-120.0f);

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
