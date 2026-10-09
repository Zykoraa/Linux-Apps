// Auto-routing must not fight an application that captures a stream itself.
// The property sets below are copied from `pactl -f json list sinks` on a real
// session: eveamp's Spotify bridge, BetterBanana's own sinks, the stream sink
// and an ALSA device.
#include "../gui/approute.h"

#include <QJsonObject>
#include <cstdio>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static QJsonObject props(const char* name, const char* cls, const char* cat, const char* app)
{
    QJsonObject o;
    o["node.name"] = name;
    if (cls) o["media.class"] = cls;
    if (cat) o["media.category"] = cat;
    if (app) o["application.name"] = app;
    return o;
}

int main()
{
    std::printf("test_approute\n");
    chk(isAppCaptureSink(props("eveamp_spotify", "Audio/Sink", "Capture", "pw-record")),
        "eveamp's capture sink is another application's");
    chk(isAppCaptureSink(props("obs_monitor", "Audio/Sink", "Capture", "OBS")),
        "so is any recorder standing in as a sink");
    chk(!isAppCaptureSink(props("bb_vaio", "Audio/Sink", "Capture", "BetterBanana")),
        "BetterBanana's own sink is not");
    chk(!isAppCaptureSink(props("bb_vaio3", "Audio/Sink", "Capture", nullptr)),
        "nor is one named bb_ whatever its owner says");
    chk(!isAppCaptureSink(props("betterbanana_stream", "Audio/Sink", nullptr, nullptr)),
        "nor the stream sink, a null sink with no category");
    chk(!isAppCaptureSink(props("alsa_output.usb-BEHRINGER", "Audio/Sink", nullptr, nullptr)),
        "nor a hardware device");
    chk(!isAppCaptureSink(props("recorder", "Stream/Input/Audio", "Capture", "pw-record")),
        "an ordinary recording stream is not a sink at all");
    chk(!isAppCaptureSink(QJsonObject()), "nothing at all is nothing");
    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
