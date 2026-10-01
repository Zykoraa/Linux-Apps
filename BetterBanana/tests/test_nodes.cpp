// Finding a saved device again: direction-aware description matching, and a
// node map that survives names being shared.
#include "../engine/nodes.h"

#include <cstdio>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

int main()
{
    const std::string ryzen = "Ryzen HD Audio Controller Pro";
    NodeMap nodes = {
        { 60, { "alsa_input.pci-0000_16_00.6.pro-input-0",   ryzen, "Audio/Source" } },
        { 61, { "alsa_output.pci-0000_16_00.6.pro-output-0", ryzen, "Audio/Sink" } },
        { 70, { "bb_spotify_source", "BetterBanana_Spotify_Cable1", "Audio/Source/Virtual" } },
        { 80, { "some_app", "Headset", "Stream/Output/Audio" } },
        { 81, { "usb_headset", "Headset", "Audio/Duplex" } },
        { 90, { "discord_capture", "", "Stream/Input/Audio" } },
        { 91, { "discord_capture", "", "Stream/Input/Audio" } },
    };

    // What happened on 2026-09-27: bus A2 had been pointed at a sink that then
    // went away, still carrying the Ryzen output's description.
    chk(resolve_node(nodes, "applio_test_feed", ryzen, Direction::Playback)
            == "alsa_output.pci-0000_16_00.6.pro-output-0",
        "a bus finds the output that shares its description with an input");
    chk(resolve_node(nodes, "gone_mic", ryzen, Direction::Capture)
            == "alsa_input.pci-0000_16_00.6.pro-input-0",
        "a strip finds the input that shares its description with an output");

    NodeMap inputs_only = { { 60, nodes.at(60) } };
    chk(resolve_node(inputs_only, "gone_out", ryzen, Direction::Playback) == "gone_out",
        "with only the wrong direction live, the bus keeps waiting for its device");

    chk(resolve_node(nodes, "alsa_output.pci-0000_16_00.6.pro-output-0", "anything",
                     Direction::Playback) == "alsa_output.pci-0000_16_00.6.pro-output-0",
        "a device that is still there is left alone");
    chk(resolve_node(nodes, "gone", "BetterBanana_Spotify_Cable1", Direction::Capture)
            == "bb_spotify_source",
        "a virtual source counts as a source");
    chk(resolve_node(nodes, "gone", "Headset", Direction::Playback) == "usb_headset",
        "a stream with the same description is never taken for the device");
    chk(resolve_node(nodes, "gone", "Headset", Direction::Capture) == "usb_headset",
        "a duplex device serves either direction");
    chk(resolve_node(nodes, "gone", "", Direction::Playback) == "gone",
        "no description, no guess");
    chk(resolve_node(nodes, "", ryzen, Direction::Playback).empty(),
        "an unassigned endpoint stays unassigned");

    // Discord runs several nodes with one name; one of them going away must not
    // make the name look gone.
    nodes.erase(90);
    chk(find_node(nodes, "discord_capture") != nullptr,
        "a shared name survives one of its nodes leaving");
    nodes.erase(91);
    chk(find_node(nodes, "discord_capture") == nullptr,
        "and goes once the last one has");

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
