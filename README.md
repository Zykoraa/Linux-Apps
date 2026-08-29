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

---

Each app builds independently — see its own README. Everything here is MIT
licensed; see [LICENSE](LICENSE).
