// Probe: can one process register an Audio/Sink and an Audio/Source and have
// PipeWire schedule both in the SAME graph cycle via node.group?
// Prints, per cycle, the clock.nsec each node observes.
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <csignal>

struct Node;
struct App {
    pw_main_loop* loop = nullptr;
    Node* sink = nullptr;
    Node* src  = nullptr;
    std::atomic<uint64_t> cycles{0};
    int printed = 0;
};

struct Node {
    App* app = nullptr;
    const char* tag = "";
    pw_filter* filter = nullptr;
    void* p[2] = {nullptr, nullptr};      // port handles (FL, FR)
    uint64_t last_nsec = 0;
    uint32_t last_dur = 0;
    std::atomic<uint64_t> calls{0};
};

static float g_ring[2][8192];
static std::atomic<uint32_t> g_ring_fill{0};

static void on_process(void* userdata, spa_io_position* pos)
{
    Node* n = static_cast<Node*>(userdata);
    const uint32_t nsamp = pos->clock.duration;
    n->last_nsec = pos->clock.nsec;
    n->last_dur  = nsamp;
    n->calls.fetch_add(1, std::memory_order_relaxed);

    if (std::strcmp(n->tag, "SINK") == 0) {
        // Capture what apps played into us, stash it.
        for (int c = 0; c < 2; ++c) {
            float* in = static_cast<float*>(pw_filter_get_dsp_buffer(n->p[c], nsamp));
            if (in && nsamp <= 8192) std::memcpy(g_ring[c], in, nsamp * sizeof(float));
            else std::memset(g_ring[c], 0, sizeof(float) * (nsamp <= 8192 ? nsamp : 8192));
        }
        g_ring_fill.store(nsamp, std::memory_order_release);
    } else {
        // Emit it again on the virtual source.
        const uint32_t have = g_ring_fill.load(std::memory_order_acquire);
        for (int c = 0; c < 2; ++c) {
            float* out = static_cast<float*>(pw_filter_get_dsp_buffer(n->p[c], nsamp));
            if (!out) continue;
            if (have >= nsamp) std::memcpy(out, g_ring[c], nsamp * sizeof(float));
            else std::memset(out, 0, nsamp * sizeof(float));
        }
    }

    App* a = n->app;
    if (n == a->src) {
        uint64_t c = a->cycles.fetch_add(1) + 1;
        if (c % 200 == 0 && a->printed < 10) {
            a->printed++;
            const long long d = (long long)a->sink->last_nsec - (long long)a->src->last_nsec;
            std::printf("cycle %5llu  quantum=%u  sink.nsec=%llu  src.nsec=%llu  delta=%lld ns  %s\n",
                        (unsigned long long)c, nsamp,
                        (unsigned long long)a->sink->last_nsec,
                        (unsigned long long)a->src->last_nsec,
                        d, d == 0 ? "<-- SAME CYCLE" : "<-- DIFFERENT");
            std::fflush(stdout);
        }
    }
}

static const pw_filter_events kEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static Node* make_node(App* app, const char* tag, const char* media_class,
                       const char* node_name, const char* desc,
                       pw_direction dir, const char* pfx)
{
    Node* n = new Node{};
    n->app = app; n->tag = tag;

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,        "Audio",
        PW_KEY_MEDIA_CATEGORY,    (dir == PW_DIRECTION_INPUT) ? "Sink" : "Source",
        PW_KEY_MEDIA_CLASS,       media_class,
        PW_KEY_NODE_NAME,         node_name,
        PW_KEY_NODE_DESCRIPTION,  desc,
        PW_KEY_NODE_GROUP,        "betterbanana",
        PW_KEY_NODE_VIRTUAL,      "true",
        PW_KEY_AUDIO_CHANNELS,    "2",
        "audio.position",         "[ FL FR ]",
        nullptr);

    n->filter = pw_filter_new_simple(pw_main_loop_get_loop(app->loop), desc, props, &kEvents, n);

    static const char* chan[2] = { "FL", "FR" };
    for (int c = 0; c < 2; ++c) {
        char pname[64];
        std::snprintf(pname, sizeof(pname), "%s_%s", pfx, chan[c]);
        n->p[c] = pw_filter_add_port(
            n->filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP,     "32 bit float mono audio",
                              PW_KEY_PORT_NAME,      pname,
                              PW_KEY_AUDIO_CHANNEL,  chan[c],
                              nullptr),
            nullptr, 0);
    }

    if (pw_filter_connect(n->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::fprintf(stderr, "FAILED to connect %s\n", desc);
        return nullptr;
    }
    return n;
}

static App g_app;
static void on_sigint(int) { if (g_app.loop) pw_main_loop_quit(g_app.loop); }

int main(int argc, char** argv)
{
    pw_init(&argc, &argv);
    g_app.loop = pw_main_loop_new(nullptr);
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    g_app.sink = make_node(&g_app, "SINK", "Audio/Sink",
                           "bb_probe_sink", "BB Probe Sink",
                           PW_DIRECTION_INPUT, "playback");
    g_app.src  = make_node(&g_app, "SRC", "Audio/Source",
                           "bb_probe_source", "BB Probe Source",
                           PW_DIRECTION_OUTPUT, "capture");
    if (!g_app.sink || !g_app.src) return 1;

    std::printf("probe running: registered sink + source in one process, node.group=betterbanana\n");
    std::fflush(stdout);
    pw_main_loop_run(g_app.loop);

    std::printf("\nsink process calls=%llu  src process calls=%llu\n",
                (unsigned long long)g_app.sink->calls.load(),
                (unsigned long long)g_app.src->calls.load());
    pw_main_loop_destroy(g_app.loop);
    pw_deinit();
    return 0;
}
