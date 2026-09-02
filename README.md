# Linux-Apps

Small Linux applications I build and actually use.

## BetterBanana

A virtual audio mixer for PipeWire, modelled on Voicemeeter Banana: routing
matrix, virtual cables, sidechain ducking, per-application routing, VBAN and a
recorder. → [full documentation](BetterBanana/)

**Install it with one command:**

```sh
curl -fsSL https://raw.githubusercontent.com/Zykoraa/Linux-Apps/main/BetterBanana/install.sh | bash
```

Installs dependencies, builds, and starts the audio engine as a background
service. Works on Arch, Debian/Ubuntu, Fedora and openSUSE. Re-run it to update.

Then run `bb-gui`.

## Glory Injector

A themed shared-object (`.so`) injector with a Tk GUI: pick a running process
(or a Hyprland window) and load a `.so` into it via the documented `gdb` →
`dlopen()` technique, verified against `/proc/<pid>/maps`. Dual-use tooling for
debugging, modding and instrumentation on processes you own. →
[full documentation](glory-injector/)

**Arch-based (prebuilt package):**

```sh
sudo pacman -U https://github.com/Zykoraa/Linux-Apps/releases/download/v1.0.0/glory-injector-1.0.0-1-any.pkg.tar.zst
```

**Any glibc distro (user install, no root):**

```sh
git clone https://github.com/Zykoraa/Linux-Apps.git
cd Linux-Apps/glory-injector && ./install.sh
```

Needs `python`, `tk` and `gdb`. Then launch **Glory Injector** from your app
menu, or run `glory-injector`.

## Mic Gain

A microphone analyzer for PipeWire that tells you which knob to turn, and which
way. Monitoring level and recording level are different knobs on a USB
interface, so your ears cannot judge the one that gets recorded; this measures
the signal on the way in and turns it into named instructions. Live meters, a
log-frequency spectrum, and a guided `--calibrate` mode that writes a settings
report. Understands an external channel strip such as a dbx 286s. →
[full documentation](mic-gain/)

**Any distro (user install, no root):**

```sh
git clone https://github.com/Zykoraa/Linux-Apps.git
cd Linux-Apps/mic-gain && ./install.sh
```

Needs `python3` and PipeWire — no third-party libraries. Then run `mic-gain`.

Installing BetterBanana from a clone installs this too, and its
**Engine → Analyse microphone** menu opens it on any strip or bus.

---

Each app builds independently — see its own README. Everything here is MIT
licensed; see [LICENSE](LICENSE).
