// bb-ctl - command line control for the betterbanana engine.
// Maps the same shared-memory segment the GUI uses.
#include "../common/protocol.h"
#include "../common/preset.h"
#include "../common/eqprofile.h"
#include "../common/fxpreset.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <csignal>

using namespace bb;

// clampf comes from dsp.h via eqprofile.h; only the integer form is local.
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Header layout (magic, version, struct_size, engine_pid) is stable across
// versions, so "quit" can find and signal the engine even when the rest of the
// struct has changed underneath us.
static pid_t engine_pid_unchecked()
{
    int fd = shm_open(kShmName, O_RDONLY, 0600);
    if (fd < 0) return -1;
    void* m = mmap(nullptr, sizeof(Shared), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    const Shared* s = static_cast<const Shared*>(m);
    pid_t pid = (s->magic.load() == kMagic) ? (pid_t)s->engine_pid.load() : -1;
    munmap(m, sizeof(Shared));
    return pid;
}

static Shared* map_shm(bool rw = true)
{
    int fd = shm_open(kShmName, rw ? O_RDWR : O_RDONLY, 0600);
    if (fd < 0) { std::fprintf(stderr, "bb-ctl: engine not running (%s)\n", kShmName); return nullptr; }
    void* m = mmap(nullptr, sizeof(Shared), PROT_READ | (rw ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return nullptr; }
    Shared* s = static_cast<Shared*>(m);
    if (!shm_compatible(s)) {
        std::fprintf(stderr, "bb-ctl: engine/tool version mismatch "
                             "(engine v%u/%uB, tool v%u/%zuB). Rebuild both and restart the engine.\n",
                     s->version.load(), s->struct_size.load(), kVersion, sizeof(Shared));
        return nullptr;
    }
    return s;
}

static float db(float lin) { return lin <= 1e-7f ? -99.9f : 20.0f * std::log10(lin); }

static void bar(float lin)
{
    const float d = db(lin);
    int n = (int)((d + 60.0f) / 60.0f * 24.0f);
    if (n < 0) n = 0;
    if (n > 24) n = 24;
    std::printf("[");
    for (int i = 0; i < 24; ++i) std::putchar(i < n ? '#' : '.');
    std::printf("] %6.1f dB", d);
}

static const char* kStripName[kStrips] = { "HW IN 1", "HW IN 2", "HW IN 3", "VAIO", "AUX" };
static const char* kBusName[kBuses]    = { "A1", "A2", "A3", "B1", "B2" };

static void usage()
{
    std::printf(
      "bb-ctl - control the betterbanana engine\n\n"
      "  status                      show routing, gains and device assignment\n"
      "  meters [n]                  print meters n times (default 1, ~10/s)\n"
      "  strip <i> gain <dB>         -60 .. +12\n"
      "  strip <i> mute|solo|mono <0|1|toggle>\n"
      "  strip <i> key <0|1>         this strip triggers the ducker\n"
      "  strip <i> duck <dB>         how far this strip drops while ducking\n"
      "  strip <i> gate|comp|aud <0..10>\n"
      "  strip <i> eq <low> <mid> <high>   dB, -12 .. +12 (the three tone knobs)\n"
      "  strip <i> eqon <0|1>        the strip's parametric EQ\n"
      "  strip <i> preamp <dB>       parametric preamp, -24 .. +12\n"
      "  strip <i> band <0..11> <gain> <freq> <Q> [type] [on]\n"
      "  strip <i> pan <-1..1>\n"
      "  strip <i> fx on|off|show\n"
      "  strip <i> fx preset <name>       voice changer preset (bb-ctl fx list)\n"
      "  strip <i> fx pitch <semitones>   -12 .. +12, 0 is off\n"
      "  strip <i> fx formant <st>|off    move formants independently of pitch\n"
      "  strip <i> fx drive <0..10> | gain <dB>\n"
      "  strip <i> fx ring <Hz> <mix>     ring modulator, 0 Hz is off\n"
      "  strip <i> fx crush <bits> <n>    bit depth (0 off), sample-hold (1 off)\n"
      "  strip <i> fx echo <ms> <fb> <mix>\n"
      "  strip <i> fx chorus <ms> <Hz> <mix>\n"
      "  strip <i> bus <A1|A2|A3|B1|B2> <0|1>\n"
      "  bus <b> gain <dB> | mute <0|1> | mono <0|1> | eq <0|1>\n"
      "  bus <b> preamp <dB>         EQ preamp, -24 .. +12\n"
      "  bus <b> band <0..11> <gain> <freq> <Q> [type] [on]\n"
      "                              type: pk ls hs hp lp notch bp\n"
      "  fx list                     list voice changer presets\n"
      "  eq list                     list built-in and saved EQ profiles\n"
      "  eq show <t>                 print an EQ as Equalizer APO text\n"
      "  eq load <t> <name|file>     apply a built-in, saved or APO/AutoEq file\n"
      "  eq save <t> <name>          save an EQ as a named profile\n"
      "  eq flat <t>                 reset an EQ to flat\n"
      "  eq preamp <t>               set the preamp so the curve cannot clip\n"
      "     <t> is a bus (A1..B2) or an input strip (s0..s4)\n"
      "  route in <1..3> <source-node-name|cable:0..2|->\n"
      "  route out <A1|A2|A3> <sink-node-name|->\n"
      "  rec file <path> | bus <A1..B2> | start | stop\n"
      "  play file <path> | start | stop | gain <dB> | loop <0|1>\n"
      "  play bus <A1|A2|A3|B1|B2> <0|1>\n"
      "  vban out <1..8> on|off | host <ip> | port <n> | name <s> | bus <A1..B2>\n"
      "  vban in  <1..8> on|off | port <n> | name <s>\n"
      "  vban apply                  reload VBAN modules\n"
      "  label strip <0..4> <name>   rename a strip (empty resets)\n"
      "  label bus <A1..B2> <name>   rename a bus\n"
      "  preset save <name|path>     save current state\n"
      "  preset load <name|path>     restore a saved state\n"
      "  preset list                 list saved presets\n"
      "  preset startup              show which preset loads when the engine starts\n"
      "  preset startup <name|none>  set it\n"
      "  duck on|off|toggle | threshold <dB> | attack <ms> | release <ms>\n"
      "  clearclip                   clear latched clip indicators\n"
      "  autostart                   show what starts at login\n"
      "  autostart engine|gui on|off set what starts at login\n"
      "  reset                       reset meters\n"
      "  quit                        stop the engine\n\n"
      "  strip index: 0=HW1 1=HW2 2=HW3 3=VAIO 4=AUX\n");
}

static int bus_index(const char* s)
{
    for (int b = 0; b < kBuses; ++b) if (strcasecmp(s, kBusName[b]) == 0) return b;
    if (s[0] >= '0' && s[0] <= '4' && s[1] == 0) return s[0] - '0';
    return -1;
}

// The voice changer, as "strip <i> fx <what> [values...]" from argv[4].
static bool set_fx(VoiceFx& p, int argc, char** argv)
{
    const std::string w = argv[4];
    auto need = [&](int n) {
        if (argc >= 5 + n) return true;
        std::fprintf(stderr, "fx %s needs %d value%s\n", w.c_str(), n, n == 1 ? "" : "s");
        return false;
    };
    FxValues v = fx_capture(p);
    if      (w == "on")  { p.on.store(1); return true; }
    else if (w == "off") { p.on.store(0); return true; }
    else if (w == "show") {
        const int idx = fx_preset_index(v);
        std::printf("on %d  preset %s\n  pitch %.2f st  formant %s  drive %.2f  gain %.2f dB\n"
                    "  ring %.1f Hz mix %.2f\n  crush %d bits, downsample %d\n"
                    "  echo %.0f ms fb %.2f mix %.2f\n  chorus %.2f ms %.2f Hz mix %.2f\n",
                    p.on.load(), idx >= 0 ? fx_presets()[idx].name : "(custom)",
                    v.pitch,
                    v.formant_on ? (std::to_string(v.formant) + " st").c_str()
                                 : "follows pitch",
                    v.drive, v.gain_db, v.ring_hz, v.ring_mix,
                    v.bits, v.downsample, v.echo_ms, v.echo_fb, v.echo_mix,
                    v.chorus_ms, v.chorus_hz, v.chorus_mix);
        return true;
    }
    else if (w == "preset") {
        if (!need(1)) return false;
        if (!fx_preset_by_name(argv[5], v)) {
            std::fprintf(stderr, "bb-ctl: no voice preset '%s' (try: bb-ctl fx list)\n", argv[5]);
            return false;
        }
        fx_apply(p, v);
        // "Off" is how you clear it, so it should not also switch the block on.
        p.on.store(std::string(argv[5]) == "Off" ? 0 : 1);
        return true;
    }
    else if (w == "pitch")  { if (!need(1)) return false; v.pitch = atof(argv[5]); }
    else if (w == "formant") {
        if (!need(1)) return false;
        if (std::string(argv[5]) == "off") { v.formant_on = false; v.formant = 0.0f; }
        else { v.formant_on = true; v.formant = atof(argv[5]); }
    }
    else if (w == "drive")  { if (!need(1)) return false; v.drive = atof(argv[5]); }
    else if (w == "gain")   { if (!need(1)) return false; v.gain_db = atof(argv[5]); }
    else if (w == "ring")   { if (!need(2)) return false; v.ring_hz = atof(argv[5]); v.ring_mix = atof(argv[6]); }
    else if (w == "crush")  { if (!need(2)) return false; v.bits = atoi(argv[5]); v.downsample = atoi(argv[6]); }
    else if (w == "echo")   { if (!need(3)) return false; v.echo_ms = atof(argv[5]); v.echo_fb = atof(argv[6]); v.echo_mix = atof(argv[7]); }
    else if (w == "chorus") { if (!need(3)) return false; v.chorus_ms = atof(argv[5]); v.chorus_hz = atof(argv[6]); v.chorus_mix = atof(argv[7]); }
    else { usage(); return false; }
    fx_apply(p, v);
    return true;
}

// "<band> <gain> <freq> <Q> [type] [on]" starting at argv[4]; shared by the
// strip and bus forms so they cannot drift apart.
static bool set_band(EqParams& p, int argc, char** argv)
{
    const int k = atoi(argv[4]);
    if (k < 0 || k >= kEqBands) {
        std::fprintf(stderr, "band must be 0..%d\n", kEqBands - 1);
        return false;
    }
    p.gain[k].store(clampf(atof(argv[5]), -24.0f, 24.0f));
    p.freq[k].store(clampf(atof(argv[6]), 10.0f, 24000.0f));
    p.q[k].store(clampf(atof(argv[7]), 0.1f, 20.0f));
    if (argc >= 9) {
        const int t = eq_type_from_tag(argv[8]);
        if (t < 0) { std::fprintf(stderr, "type: pk ls hs hp lp notch bp\n"); return false; }
        p.type[k].store(t);
    }
    if (argc >= 10) p.band_on[k].store(atoi(argv[9]) ? 1 : 0);
    return true;
}

// An EQ block belongs to a bus (A1..B2) or to an input strip (s0..s4). Both
// are the same struct, so every eq subcommand works on either.
static EqParams* eq_target(Shared* s, const char* name, std::string& label)
{
    const int b = bus_index(name);
    if (b >= 0) { label = kBusName[b]; return &s->bus[b].eq; }
    if ((name[0] == 's' || name[0] == 'S') && name[1] >= '0' && name[1] <= '9' && name[2] == 0) {
        const int i = name[1] - '0';
        if (i < kStrips) { label = kStripName[i]; return &s->strip[i].eq; }
    }
    return nullptr;
}

static void send_cmd(Shared* s, Command c)
{
    s->cmd.store(c, std::memory_order_relaxed);
    s->cmd_seq.fetch_add(1, std::memory_order_release);
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }

    // Start-at-login needs no running engine, so handle it before mapping.
    if (std::string(argv[1]) == "autostart") {
        const std::string home = getenv("HOME") ? getenv("HOME") : ".";
        const std::string gui_desktop = home + "/.config/autostart/betterbanana.desktop";
        auto unit_state = [] {
            FILE* f = popen("systemctl --user is-enabled betterbanana-engine.service 2>/dev/null", "r");
            if (!f) return std::string("unknown");
            char buf[64] = {};
            if (!fgets(buf, sizeof(buf), f)) { pclose(f); return std::string("not-found"); }
            pclose(f);
            std::string v(buf);
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
            return v.empty() ? std::string("not-found") : v;
        };
        if (argc == 2) {
            std::printf("engine  %s\n", unit_state().c_str());
            std::printf("gui     %s\n",
                access(gui_desktop.c_str(), F_OK) == 0 ? "enabled" : "disabled");
            return 0;
        }
        if (argc < 4) { usage(); return 1; }
        const std::string what = argv[2], val = argv[3];
        const bool on = (val == "on" || val == "1" || val == "true");
        if (what == "engine") {
            std::string cmd = "systemctl --user ";
            cmd += on ? "enable" : "disable";
            cmd += " betterbanana-engine.service";
            if (std::system(cmd.c_str()) != 0) {
                std::fprintf(stderr, "bb-ctl: could not %s the service; is it installed?\n",
                             on ? "enable" : "disable");
                return 1;
            }
        } else if (what == "gui") {
            if (on) {
                std::string dir = home + "/.config/autostart";
                mkdir((home + "/.config").c_str(), 0755);
                mkdir(dir.c_str(), 0755);
                FILE* f = fopen(gui_desktop.c_str(), "w");
                if (!f) { perror("bb-ctl"); return 1; }
                fputs("[Desktop Entry]\nType=Application\nName=BetterBanana\n"
                      "Comment=Virtual audio mixer\nExec=bb-gui\nIcon=betterbanana\n"
                      "Terminal=false\nX-GNOME-Autostart-enabled=true\n", f);
                fclose(f);
            } else {
                unlink(gui_desktop.c_str());
            }
        } else { usage(); return 1; }
        std::printf("%s autostart %s\n", what.c_str(), on ? "enabled" : "disabled");
        return 0;
    }

    // Catalogues read nothing but built-ins and the profile directory, so they
    // answer whether or not an engine is running - which is exactly when you
    // want to look one up.
    if (std::string(argv[1]) == "fx" && argc >= 3 && std::string(argv[2]) == "list") {
        std::printf("voice changer presets:\n");
        for (const FxPreset& f : fx_presets()) std::printf("  %s\n", f.name);
        std::printf("\nApply one with:  bb-ctl strip <i> fx preset \"<name>\"\n"
                    "A telephone or radio voice is an EQ recipe rather than an effect:\n"
                    "  bb-ctl strip 0 band 0 0 300 0.7 hp\n"
                    "  bb-ctl strip 0 band 1 0 3400 0.7 lp\n"
                    "  bb-ctl strip 0 eqon 1\n");
        return 0;
    }
    if (std::string(argv[1]) == "eq" && argc >= 3 && std::string(argv[2]) == "list") {
        std::printf("built-in:\n");
        for (const EqFactoryPreset& fp : eq_factory_presets())
            std::printf("  %s\n", fp.name);
        DIR* d = opendir(eq_profile_dir().c_str());
        if (!d) { std::printf("\nno saved profiles yet (%s)\n", eq_profile_dir().c_str()); return 0; }
        std::printf("\nsaved in %s:\n", eq_profile_dir().c_str());
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)
                std::printf("  %s\n", n.substr(0, n.size() - 4).c_str());
        }
        closedir(d);
        return 0;
    }

    // Handled before the compatibility check: stopping a stale engine is
    // exactly what you need after a version bump.
    if (std::string(argv[1]) == "quit") {
        const pid_t pid = engine_pid_unchecked();
        if (pid <= 0) { std::fprintf(stderr, "bb-ctl: no engine running\n"); return 1; }
        if (kill(pid, SIGTERM) != 0) { perror("bb-ctl: kill"); return 1; }
        std::printf("stopped engine pid %d\n", pid);
        return 0;
    }

    Shared* s = map_shm();
    if (!s) return 1;
    const std::string cmd = argv[1];

    if (cmd == "status") {
        std::printf("engine pid %d   heartbeat %u   rate %.0f   dsp load %.1f%%\n\n",
                    s->engine_pid.load(), s->engine_heartbeat.load(),
                    s->samplerate.load(), s->dsp_load.load() / 10.0);
        char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
        uint32_t seq;
        for (int t = 0; t < 8 && !routing_read(s->routing, seq, hw, out); ++t) {}
        char lstrip[kStrips][kLabelLen], lbus[kBuses][kLabelLen];
        bool lok = false;
        for (int t = 0; t < 16 && !lok; ++t) lok = labels_read(s->labels, lstrip, lbus);
        std::printf("%-12s %7s %5s %5s %5s  %-5s %-5s %-5s %-3s  routing\n",
                    "STRIP", "GAIN", "MUTE", "SOLO", "MONO", "GATE", "COMP", "AUD", "EQ");
        for (int i = 0; i < kStrips; ++i) {
            StripParams& p = s->strip[i];
            std::printf("%-12s %6.1f  %5d %5d %5d  %5.1f %5.1f %5.1f %-3s ",
                (lok && lstrip[i][0]) ? lstrip[i] : kStripName[i], p.gain_db.load(), p.mute.load(), p.solo.load(), p.mono.load(),
                p.gate.load(), p.comp.load(), p.audibility.load(),
                p.eq.on.load() ? "ON" : "-");
            for (int b = 0; b < kBuses; ++b)
                std::printf("%s%s ", kBusName[b], p.bus_on[b].load() ? "*" : "-");
            if (i < kHwStrips) {
                // Render "cable:N" the way the GUI labels it.
                if (std::strncmp(hw[i], kCablePrefix, std::strlen(kCablePrefix)) == 0)
                    std::printf("  <- BetterBanana Cable %d", atoi(hw[i] + std::strlen(kCablePrefix)) + 1);
                else
                    std::printf("  <- %s", hw[i][0] ? hw[i] : "(unassigned)");
            }
            std::printf("\n");
        }
        std::printf("\n%-12s %7s %5s %5s %5s   device\n", "BUS", "GAIN", "MUTE", "MONO", "EQ");
        for (int b = 0; b < kBuses; ++b) {
            BusParams& p = s->bus[b];
            std::printf("%-12s %6.1f  %5d %5d %5d   %s\n",
                (lok && lbus[b][0]) ? lbus[b] : kBusName[b], p.gain_db.load(),
                p.mute.load(), p.mono.load(), p.eq.on.load(),
                b < kPhysBuses ? (out[b][0] ? out[b] : "(unassigned)") : "(virtual source)");
        }
        const char* st[] = { "idle", "RECORDING", "PLAYING" };
        std::printf("\nDUCKER    %s  threshold %.1f dB  attack %.0f ms  release %.0f ms  env %.2f\n",
            s->duck_enabled.load() ? "ON " : "off", s->duck_threshold_db.load(),
            s->duck_attack_ms.load(), s->duck_release_ms.load(), s->meters.duck_env.load());
        std::printf("          ");
        for (int i = 0; i < kStrips; ++i) {
            const bool k = s->strip[i].duck_key.load() != 0;
            const float d = s->strip[i].duck_depth_db.load();
            if (k) std::printf("[%s=KEY] ", (lok && lstrip[i][0]) ? lstrip[i] : kStripName[i]);
            else if (d < -0.05f) std::printf("[%s %.0fdB] ", (lok && lstrip[i][0]) ? lstrip[i] : kStripName[i], d);
        }
        std::printf("\n");

        std::printf("\nRECORDER  %s   src=%s  written=%u  played=%u/%u  gain=%.1f  loop=%d\n",
            st[clampi(s->rec.state.load(), 0, 2)], kBusName[clampi(s->rec.source_bus.load(), 0, kBuses-1)],
            s->rec.frames_written.load(), s->rec.frames_played.load(),
            s->rec.total_frames.load(), s->rec.gain_db.load(), s->rec.loop.load());
        std::printf("          play routing: ");
        for (int b = 0; b < kBuses; ++b) std::printf("%s%s ", kBusName[b], s->rec.bus_on[b].load() ? "*" : "-");
        std::printf("\n");

        std::printf("\nVBAN      %-4s %-9s %-18s %-6s %s\n", "#", "DIR", "NAME", "PORT", "DETAIL");
        for (int i = 0; i < kVbanStreams; ++i) {
            const VbanOutCfg& o = s->vban.out[i];
            if (o.enabled) std::printf("          %-4d %-9s %-18s %-6d -> %s  from %s\n",
                i + 1, "out", o.name, o.port, o.host[0] ? o.host : "(no host)",
                kBusName[clampi(o.source_bus, 0, kBuses - 1)]);
        }
        for (int i = 0; i < kVbanStreams; ++i) {
            const VbanInCfg& n = s->vban.in[i];
            if (n.enabled) std::printf("          %-4d %-9s %-18s %-6d <- source bb_vban_in_%d\n",
                i + 1, "in", n.name, n.port, i + 1);
        }
        return 0;
    }

    if (cmd == "meters") {
        const int n = argc > 2 ? atoi(argv[2]) : 1;
        for (int k = 0; k < n; ++k) {
            if (k) usleep(100000);
            std::printf("\n");
            for (int i = 0; i < kStrips; ++i) {
                std::printf("  %-7s pre ", kStripName[i]);
                bar(std::max(s->meters.strip_pre[i][0].load(), s->meters.strip_pre[i][1].load()));
                std::printf("   post ");
                bar(std::max(s->meters.strip_post[i][0].load(), s->meters.strip_post[i][1].load()));
                std::printf("\n");
            }
            for (int b = 0; b < kBuses; ++b) {
                std::printf("  BUS %-3s     ", kBusName[b]);
                bar(std::max(s->meters.bus_out[b][0].load(), s->meters.bus_out[b][1].load()));
                std::printf("\n");
            }
        }
        return 0;
    }

    if (cmd == "strip" && argc >= 4) {
        const int i = atoi(argv[2]);
        if (i < 0 || i >= kStrips) { std::fprintf(stderr, "strip 0..%d\n", kStrips - 1); return 1; }
        StripParams& p = s->strip[i];
        const std::string w = argv[3];
        // "toggle" flips the current value; that is what a global hotkey binds to.
        auto flag = [&](ai& target, const char* v) {
            const bool tog = std::strcmp(v, "toggle") == 0;
            target.store(tog ? (target.load() ? 0 : 1) : (atoi(v) ? 1 : 0));
            // Only a toggle reports back, so a hotkey can show the new state
            // while scripted 0/1 calls stay quiet.
            if (tog) std::printf("%d\n", target.load());
        };
        if      (w == "gain" && argc >= 5) p.gain_db.store(clampf(atof(argv[4]), -60.0f, 12.0f));
        else if (w == "mute" && argc >= 5) flag(p.mute, argv[4]);
        else if (w == "solo" && argc >= 5) flag(p.solo, argv[4]);
        else if (w == "mono" && argc >= 5) flag(p.mono, argv[4]);
        else if (w == "key"  && argc >= 5) flag(p.duck_key, argv[4]);
        else if (w == "duck" && argc >= 5) p.duck_depth_db.store(clampf(atof(argv[4]), -60.0f, 0.0f));
        else if (w == "gate" && argc >= 5) p.gate.store(clampf(atof(argv[4]), 0.0f, 10.0f));
        else if (w == "comp" && argc >= 5) p.comp.store(clampf(atof(argv[4]), 0.0f, 10.0f));
        else if (w == "aud"  && argc >= 5) p.audibility.store(clampf(atof(argv[4]), 0.0f, 10.0f));
        else if (w == "pan"  && argc >= 5) p.pan_x.store(clampf(atof(argv[4]), -1.0f, 1.0f));
        else if (w == "eq"   && argc >= 7) {
            p.eq_low.store (clampf(atof(argv[4]), -12.0f, 12.0f));
            p.eq_mid.store (clampf(atof(argv[5]), -12.0f, 12.0f));
            p.eq_high.store(clampf(atof(argv[6]), -12.0f, 12.0f));
        }
        else if (w == "eqon"   && argc >= 5) p.eq.on.store(atoi(argv[4]) ? 1 : 0);
        else if (w == "preamp" && argc >= 5) p.eq.preamp_db.store(clampf(atof(argv[4]), -24.0f, 12.0f));
        else if (w == "band" && argc >= 8) {
            if (!set_band(p.eq, argc, argv)) return 1;
        }
        else if (w == "fx" && argc >= 5) {
            if (!set_fx(p.fx, argc, argv)) return 1;
        }
        else if (w == "bus" && argc >= 6) {
            const int b = bus_index(argv[4]);
            if (b < 0) { std::fprintf(stderr, "bus must be A1..A3,B1,B2\n"); return 1; }
            p.bus_on[b].store(atoi(argv[5]) ? 1 : 0);
        }
        else { usage(); return 1; }
        return 0;
    }

    if (cmd == "bus" && argc >= 5) {
        const int b = bus_index(argv[2]);
        if (b < 0) { std::fprintf(stderr, "bus must be A1..A3,B1,B2\n"); return 1; }
        BusParams& p = s->bus[b];
        const std::string w = argv[3];
        if      (w == "gain") p.gain_db.store(clampf(atof(argv[4]), -60.0f, 12.0f));
        else if (w == "mute") p.mute.store(atoi(argv[4]) ? 1 : 0);
        else if (w == "mono") p.mono.store(atoi(argv[4]) ? 1 : 0);
        else if (w == "eq")   p.eq.on.store(atoi(argv[4]) ? 1 : 0);
        else if (w == "preamp") p.eq.preamp_db.store(clampf(atof(argv[4]), -24.0f, 12.0f));
        else if (w == "band" && argc >= 8) {
            if (!set_band(p.eq, argc, argv)) return 1;
        }
        else { usage(); return 1; }
        return 0;
    }

    if (cmd == "eq" && argc >= 3) {
        const std::string w = argv[2];

        // A bare name is a profile: a saved one first, then a built-in. Anything
        // with a slash or a .txt suffix is taken as a literal file, so an AutoEq
        // download can be applied straight from where it landed.
        auto resolve = [](const std::string& n, EqProfile& out) -> bool {
            const bool literal = n.find('/') != std::string::npos
                              || (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0);
            if (literal) return eq_read_file(out, n.c_str());
            const std::string path = eq_profile_dir() + "/" + n + ".txt";
            if (eq_read_file(out, path.c_str())) { out.name = n; return true; }
            return eq_factory_profile(n, out);
        };

        // Everything past "list" names a target first.
        if (argc < 4) { usage(); return 1; }
        std::string label;
        EqParams* eq = eq_target(s, argv[3], label);
        if (!eq) {
            std::fprintf(stderr, "target must be A1..A3, B1, B2, or s0..s%d\n", kStrips - 1);
            return 1;
        }

        if (w == "show") {
            std::printf("%s", eq_format_apo(eq_capture(*eq, label)).c_str());
            return 0;
        }
        if (w == "flat") {
            EqProfile flat;
            eq_apply(*eq, flat);
            std::printf("%s EQ reset to flat\n", label.c_str());
            return 0;
        }
        if (w == "preamp") {
            const float pa = eq_suggest_preamp(eq_capture(*eq));
            eq->preamp_db.store(pa);
            std::printf("%s preamp %.1f dB\n", label.c_str(), pa);
            return 0;
        }
        if (w == "load" && argc >= 5) {
            EqProfile prof;
            if (!resolve(argv[4], prof)) {
                std::fprintf(stderr, "bb-ctl: no EQ profile '%s' (try: bb-ctl eq list)\n", argv[4]);
                return 1;
            }
            eq_apply(*eq, prof);
            eq->on.store(1);
            std::printf("%s <- %s (%zu bands, preamp %.1f dB)\n", label.c_str(),
                        prof.name.empty() ? argv[4] : prof.name.c_str(),
                        prof.bands.size(), prof.preamp);
            return 0;
        }
        if (w == "save" && argc >= 5) {
            mkdir(preset_dir().c_str(), 0755);
            mkdir(eq_profile_dir().c_str(), 0755);
            const std::string path = eq_profile_dir() + "/" + argv[4] + ".txt";
            if (!eq_write_file(eq_capture(*eq, argv[4]), path.c_str())) {
                std::fprintf(stderr, "bb-ctl: cannot write %s\n", path.c_str());
                return 1;
            }
            std::printf("saved %s\n", path.c_str());
            return 0;
        }
        usage();
        return 1;
    }

    if (cmd == "route" && argc >= 5) {
        const std::string dir = argv[2];
        const char* name = argv[4];
        if (std::strcmp(name, "-") == 0) name = "";
        routing_write_begin(s->routing);
        if (dir == "in") {
            const int i = atoi(argv[3]) - 1;
            if (i < 0 || i >= kHwStrips) { routing_write_end(s->routing); std::fprintf(stderr, "in 1..3\n"); return 1; }
            std::snprintf(s->routing.hw_in[i], kNameLen, "%s", name);
        } else if (dir == "out") {
            const int b = bus_index(argv[3]);
            if (b < 0 || b >= kPhysBuses) { routing_write_end(s->routing); std::fprintf(stderr, "out A1..A3\n"); return 1; }
            std::snprintf(s->routing.bus_out[b], kNameLen, "%s", name);
        } else { routing_write_end(s->routing); usage(); return 1; }
        routing_write_end(s->routing);
        return 0;
    }

    if (cmd == "rec" && argc >= 3) {
        const std::string w = argv[2];
        if (w == "file" && argc >= 4) {
            s->rec.cfg_seq.fetch_add(1, std::memory_order_acq_rel);
            std::snprintf(s->rec.rec_path, kNameLen, "%s", argv[3]);
            s->rec.cfg_seq.fetch_add(1, std::memory_order_release);
        } else if (w == "bus" && argc >= 4) {
            const int b = bus_index(argv[3]);
            if (b < 0) { std::fprintf(stderr, "bus must be A1..A3,B1,B2\n"); return 1; }
            s->rec.source_bus.store(b);
        } else if (w == "start") send_cmd(s, kCmdRecStart);
        else if (w == "stop")    send_cmd(s, kCmdRecStop);
        else { usage(); return 1; }
        return 0;
    }

    if (cmd == "play" && argc >= 3) {
        const std::string w = argv[2];
        if (w == "file" && argc >= 4) {
            s->rec.cfg_seq.fetch_add(1, std::memory_order_acq_rel);
            std::snprintf(s->rec.play_path, kNameLen, "%s", argv[3]);
            s->rec.cfg_seq.fetch_add(1, std::memory_order_release);
        } else if (w == "start") send_cmd(s, kCmdPlayStart);
        else if (w == "stop")    send_cmd(s, kCmdPlayStop);
        else if (w == "gain" && argc >= 4) s->rec.gain_db.store(clampf(atof(argv[3]), -60.0f, 12.0f));
        else if (w == "loop" && argc >= 4) s->rec.loop.store(atoi(argv[3]) ? 1 : 0);
        else if (w == "bus"  && argc >= 5) {
            const int b = bus_index(argv[3]);
            if (b < 0) { std::fprintf(stderr, "bus must be A1..A3,B1,B2\n"); return 1; }
            s->rec.bus_on[b].store(atoi(argv[4]) ? 1 : 0);
        } else { usage(); return 1; }
        return 0;
    }

    if (cmd == "vban") {
        if (argc >= 3 && std::string(argv[2]) == "apply") { send_cmd(s, kCmdVbanReload); return 0; }
        if (argc < 5) { usage(); return 1; }
        const std::string dir = argv[2];
        const int i = atoi(argv[3]) - 1;
        if (i < 0 || i >= kVbanStreams) { std::fprintf(stderr, "stream 1..%d\n", kVbanStreams); return 1; }
        const std::string w = argv[4];
        s->vban.seq.fetch_add(1, std::memory_order_acq_rel);
        if (dir == "out") {
            VbanOutCfg& o = s->vban.out[i];
            if      (w == "on")  o.enabled = 1;
            else if (w == "off") o.enabled = 0;
            else if (w == "host" && argc >= 6) std::snprintf(o.host, sizeof(o.host), "%s", argv[5]);
            else if (w == "port" && argc >= 6) o.port = atoi(argv[5]);
            else if (w == "name" && argc >= 6) std::snprintf(o.name, sizeof(o.name), "%s", argv[5]);
            else if (w == "bus"  && argc >= 6) { const int b = bus_index(argv[5]); if (b >= 0) o.source_bus = b; }
        } else if (dir == "in") {
            VbanInCfg& n = s->vban.in[i];
            if      (w == "on")  n.enabled = 1;
            else if (w == "off") n.enabled = 0;
            else if (w == "port" && argc >= 6) n.port = atoi(argv[5]);
            else if (w == "name" && argc >= 6) std::snprintf(n.name, sizeof(n.name), "%s", argv[5]);
        }
        s->vban.seq.fetch_add(1, std::memory_order_release);
        return 0;
    }

    if (cmd == "label" && argc >= 4) {
        const std::string what = argv[2];
        const char* text = argc >= 5 ? argv[4] : "";
        int idx = -1;
        bool strip = (what == "strip");
        if (strip) idx = atoi(argv[3]);
        else if (what == "bus") idx = bus_index(argv[3]);
        else { usage(); return 1; }
        if (idx < 0 || idx >= (strip ? kStrips : kBuses)) {
            std::fprintf(stderr, "index out of range\n"); return 1;
        }
        s->labels.seq.fetch_add(1, std::memory_order_acq_rel);
        std::snprintf(strip ? s->labels.strip[idx] : s->labels.bus[idx], kLabelLen, "%s", text);
        s->labels.seq.fetch_add(1, std::memory_order_release);
        return 0;
    }

    if (cmd == "preset" && argc >= 3) {
        const std::string w = argv[2];
        // A bare name lands in the preset directory; anything with a slash or
        // a .bbp suffix is taken as a literal path.
        auto resolve = [](const char* n) {
            std::string v = n;
            if (v.find('/') != std::string::npos) return v;
            return presets_path() + "/" + v + ".bbp";
        };
        if (w == "list") {
            DIR* d = opendir(presets_path().c_str());
            if (!d) { std::printf("no presets yet (%s)\n", presets_path().c_str()); return 0; }
            std::printf("presets in %s:\n", presets_path().c_str());
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bbp") == 0)
                    std::printf("  %s\n", n.substr(0, n.size() - 4).c_str());
            }
            closedir(d);
            return 0;
        }
        if (w == "startup") {
            if (argc < 4) {
                const std::string name = startup_preset_name();
                if (name.empty()) std::printf("no startup preset set\n");
                else std::printf("%s  (%s)\n", name.c_str(), preset_path_for(name).c_str());
                return 0;
            }
            const std::string want = std::string(argv[3]) == "none" ? std::string() : argv[3];
            // Refuse to point at something that is not there: the error belongs
            // here, not in the engine log at next login.
            if (!want.empty() && access(preset_path_for(want).c_str(), R_OK) != 0) {
                std::fprintf(stderr, "bb-ctl: no preset '%s' (%s)\n",
                             want.c_str(), preset_path_for(want).c_str());
                return 1;
            }
            if (!set_startup_preset_name(want)) {
                std::fprintf(stderr, "bb-ctl: cannot write %s\n", startup_marker_path().c_str());
                return 1;
            }
            std::printf(want.empty() ? "startup preset cleared\n" : "startup preset: %s\n",
                        want.c_str());
            return 0;
        }
        if (argc < 4) { usage(); return 1; }
        const std::string path = resolve(argv[3]);
        if (w == "save") {
            mkdir(preset_dir().c_str(), 0755);
            mkdir(presets_path().c_str(), 0755);
            if (!save_preset(s, path.c_str())) {
                std::fprintf(stderr, "bb-ctl: cannot write %s\n", path.c_str());
                return 1;
            }
            std::printf("saved %s\n", path.c_str());
        } else if (w == "load") {
            if (!load_preset(s, path.c_str())) {
                std::fprintf(stderr, "bb-ctl: cannot read %s\n", path.c_str());
                return 1;
            }
            send_cmd(s, kCmdVbanReload);
            std::printf("loaded %s\n", path.c_str());
        } else { usage(); return 1; }
        return 0;
    }

    if (cmd == "duck" && argc >= 3) {
        const std::string w = argv[2];
        if      (w == "on")     s->duck_enabled.store(1);
        else if (w == "off")    s->duck_enabled.store(0);
        else if (w == "toggle") s->duck_enabled.store(s->duck_enabled.load() ? 0 : 1);
        else if (w == "threshold" && argc >= 4) s->duck_threshold_db.store(clampf(atof(argv[3]), -80.0f, 0.0f));
        else if (w == "attack"    && argc >= 4) s->duck_attack_ms.store(clampf(atof(argv[3]), 1.0f, 500.0f));
        else if (w == "release"   && argc >= 4) s->duck_release_ms.store(clampf(atof(argv[3]), 10.0f, 5000.0f));
        else { usage(); return 1; }
        return 0;
    }

    if (cmd == "clearclip") { send_cmd(s, kCmdClearClip); return 0; }
    if (cmd == "reset") { send_cmd(s, kCmdResetMeters); return 0; }

    usage();
    return 1;
}
