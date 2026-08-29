// Probe 2: pw_stream-based virtual sink + virtual source in ONE process,
// sharing node.group. Verifies (a) pipewire-pulse exposes them as real
// devices, (b) both run in the same graph cycle, (c) audio survives the trip.
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <csignal>
#include <cmath>

static constexpr uint32_t kRate = 48000, kChan = 2, kRing = 16384;

struct Ep;
struct App {
    pw_main_loop* loop = nullptr;
    Ep* sink = nullptr;
    Ep* src  = nullptr;
    std::atomic<int> printed{0};
};

struct Ep {
    App* app; const char* tag;
    pw_stream* stream = nullptr;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> nsec{0};
    std::atomic<uint32_t> frames{0};
};

// Interleaved stereo ring, sink -> source.
static float    g_ring[kRing * kChan];
static std::atomic<uint32_t> g_wr{0}, g_rd{0};
static std::atomic<uint64_t> g_in_frames{0}, g_out_frames{0}, g_underruns{0};
static std::atomic<uint32_t> g_max_fill{0};

static void on_process(void* data)
{
    Ep* e = static_cast<Ep*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(e->stream);
    if (!b) return;
    spa_data& d = b->buffer->datas[0];
    e->calls.fetch_add(1, std::memory_order_relaxed);
    e->nsec.store(pw_stream_get_nsec(e->stream), std::memory_order_relaxed);

    const bool is_sink = e->tag[0] == 'S' && e->tag[1] == 'I';

    if (is_sink) {
        const uint32_t n = d.chunk->size / (sizeof(float) * kChan);
        const float* in = static_cast<const float*>(d.data);
        e->frames.store(n, std::memory_order_relaxed);
        if (in) {
            uint32_t wr = g_wr.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n * kChan; ++i) g_ring[(wr * kChan + i) % (kRing * kChan)] = in[i];
            g_wr.store((wr + n) % kRing, std::memory_order_release);
            g_in_frames.fetch_add(n, std::memory_order_relaxed);
        }
        b->buffer->datas[0].chunk->size = d.chunk->size;
    } else {
        uint32_t n = b->requested ? (uint32_t)b->requested
                                  : d.maxsize / (sizeof(float) * kChan);
        if (n * kChan * sizeof(float) > d.maxsize) n = d.maxsize / (sizeof(float) * kChan);
        float* out = static_cast<float*>(d.data);
        e->frames.store(n, std::memory_order_relaxed);

        const uint32_t wr = g_wr.load(std::memory_order_acquire);
        uint32_t rd = g_rd.load(std::memory_order_relaxed);
        uint32_t avail = (wr + kRing - rd) % kRing;
        if (avail > g_max_fill.load(std::memory_order_relaxed)) g_max_fill.store(avail, std::memory_order_relaxed);

        if (out) {
            if (avail >= n) {
                for (uint32_t i = 0; i < n * kChan; ++i) out[i] = g_ring[(rd * kChan + i) % (kRing * kChan)];
                g_rd.store((rd + n) % kRing, std::memory_order_release);
                g_out_frames.fetch_add(n, std::memory_order_relaxed);
            } else {
                std::memset(out, 0, n * kChan * sizeof(float));
                g_underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        d.chunk->offset = 0;
        d.chunk->stride = sizeof(float) * kChan;
        d.chunk->size   = n * kChan * sizeof(float);

        // Report cycle alignment periodically.
        App* a = e->app;
        uint64_t c = e->calls.load(std::memory_order_relaxed);
        if (c % 300 == 0 && a->printed.load() < 8) {
            a->printed.fetch_add(1);
            long long delta = (long long)a->sink->nsec.load() - (long long)a->src->nsec.load();
            std::printf("cyc %5llu | sink n=%u src n=%u | dt=%+lld ns | ringfill=%u | under=%llu\n",
                        (unsigned long long)c, a->sink->frames.load(), n, delta, avail,
                        (unsigned long long)g_underruns.load());
            std::fflush(stdout);
        }
    }
    pw_stream_queue_buffer(e->stream, b);
}

static void on_state(void* data, pw_stream_state old, pw_stream_state st, const char* err)
{
    Ep* e = static_cast<Ep*>(data);
    std::printf("[%s] %s -> %s%s%s\n", e->tag, pw_stream_state_as_string(old),
                pw_stream_state_as_string(st), err ? " err=" : "", err ? err : "");
    std::fflush(stdout);
}

static const pw_stream_events kEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .process = on_process,
};

static Ep* make_ep(App* app, const char* tag, const char* media_class,
                   const char* name, const char* desc, pw_direction dir)
{
    Ep* e = new Ep{app, tag};
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,       "Audio",
        PW_KEY_MEDIA_CATEGORY,   (dir == PW_DIRECTION_INPUT) ? "Capture" : "Playback",
        PW_KEY_MEDIA_CLASS,      media_class,
        PW_KEY_MEDIA_ROLE,       "Production",
        PW_KEY_NODE_NAME,        name,
        PW_KEY_NODE_DESCRIPTION, desc,
        PW_KEY_NODE_GROUP,       "betterbanana",
        PW_KEY_NODE_VIRTUAL,     "true",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        nullptr);
    e->stream = pw_stream_new_simple(pw_main_loop_get_loop(app->loop), desc, props, &kEvents, e);

    uint8_t buf[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChan;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1] = { spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info) };

    int r = pw_stream_connect(e->stream, dir, PW_ID_ANY,
        (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (r < 0) { std::fprintf(stderr, "connect %s failed: %s\n", desc, spa_strerror(r)); return nullptr; }
    return e;
}

static App g_app;
static void on_sig(int) { if (g_app.loop) pw_main_loop_quit(g_app.loop); }

int main(int argc, char** argv)
{
    pw_init(&argc, &argv);
    g_app.loop = pw_main_loop_new(nullptr);
    std::signal(SIGINT, on_sig); std::signal(SIGTERM, on_sig);

    g_app.sink = make_ep(&g_app, "SINK", "Audio/Sink",   "bb_probe_sink",   "BB Probe Sink",   PW_DIRECTION_INPUT);
    g_app.src  = make_ep(&g_app, "SRC",  "Audio/Source", "bb_probe_source", "BB Probe Source", PW_DIRECTION_OUTPUT);
    if (!g_app.sink || !g_app.src) return 1;

    std::printf("probe2: pw_stream sink+source, node.group=betterbanana\n"); std::fflush(stdout);
    pw_main_loop_run(g_app.loop);

    std::printf("\nsink calls=%llu src calls=%llu | in=%llu out=%llu frames | underruns=%llu | peak ringfill=%u\n",
        (unsigned long long)g_app.sink->calls.load(), (unsigned long long)g_app.src->calls.load(),
        (unsigned long long)g_in_frames.load(), (unsigned long long)g_out_frames.load(),
        (unsigned long long)g_underruns.load(), g_max_fill.load());
    pw_main_loop_destroy(g_app.loop); pw_deinit();
    return 0;
}
