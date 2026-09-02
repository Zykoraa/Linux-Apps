# BetterBanana

A virtual audio mixer for Linux, built on PipeWire and modelled on
[Voicemeeter Banana](https://vb-audio.com/Voicemeeter/banana.htm): 5 input
strips (3 hardware + 2 virtual) feeding a 5×5 routing matrix into 5 buses
(3 hardware + 2 virtual), with per-strip gate, compressor, EQ, Intellipan and
fader.

Beyond the original it adds three standalone virtual cables, sidechain ducking,
per-application routing with remembered rules, switchable colour themes, and a
tape deck. It is an independent implementation, not affiliated with VB-Audio.

**Features**

- 5×5 routing matrix, per-bus solo, per-strip gate / compressor / 3-band EQ
- 3 virtual cables assignable to any hardware strip, plus VAIO and AUX
- Sidechain ducking: music steps back while you talk
- 12-band parametric EQ on **every strip and every bus**: shelves, pass filters,
  preamp, draggable curve over a live spectrum analyser
- Voice changer per input strip: independent pitch **and formant** shifting,
  drive, ring mod, bit crush, chorus and echo, with presets
- Calibrates itself to your voice: records you, measures your pitch, works out
  the shift, and plays it back so you can judge it
- Undo and redo across the whole mixer, however a change was made
- Named EQ profiles, 12 built-in presets, and Equalizer APO / Peace import
- Headphone corrections for ~8850 models from the AutoEq database, searchable
- Per-application routing that survives the app restarting
- Strip settings that follow a microphone from device to device, when you ask
- Opens the mic-gain analyzer on any strip or bus, straight from the menu
- Recorder, 8×8 VBAN network audio, presets, 10 colour themes

## Install

One command, on any systemd + PipeWire distribution:

```sh
curl -fsSL https://raw.githubusercontent.com/Zykoraa/Linux-Apps/main/BetterBanana/install.sh | bash
```

It installs the dependencies for your distribution (Arch, Debian/Ubuntu, Fedora
or openSUSE), builds, installs into `~/.local/bin`, and starts the audio engine
as a systemd user service. Re-run it any time to update.

Prefer to read it first — always sensible before piping anything to a shell:

```sh
curl -fsSL -O https://raw.githubusercontent.com/Zykoraa/Linux-Apps/main/BetterBanana/install.sh
less install.sh && bash install.sh
```

Then run `bb-gui`. Point an application's output at **BetterBanana VAIO** (or a
Cable), and set your chat app's microphone to **BetterBanana Out B1**.

## Getting it onto another Linux machine

    make dist        # -> betterbanana-0.1.0.tar.gz, self-contained source

Copy that tarball over. Nothing in it is tied to this machine: the build asks Qt
where its `moc` lives rather than hardcoding a path, and `make check-deps` runs
first and names anything missing instead of failing halfway through a compile.

**Arch / CachyOS / Manjaro** — build a real package:

    cp packaging/PKGBUILD . && makepkg -si

That installs to `/usr/bin`, adds the icon and desktop entry, and ships the
systemd user unit to `/usr/lib/systemd/user`. Each user then runs their own
engine with `systemctl --user enable --now betterbanana-engine`.

**Any other distribution** — build and install into your home directory:

    tar xzf betterbanana-0.1.0.tar.gz && cd betterbanana-0.1.0
    make && make gui && make install

Dependencies:

| Distribution | Command |
|---|---|
| Arch / CachyOS | `pacman -S base-devel pipewire qt6-base qt6-tools libsndfile libpulse` |
| Debian / Ubuntu | `apt install build-essential pkg-config libpipewire-0.3-dev qt6-base-dev libsndfile1-dev pulseaudio-utils` |
| Fedora | `dnf install gcc-c++ pkgconf pipewire-devel qt6-qtbase-devel libsndfile-devel pulseaudio-utils` |

`pactl` is a runtime dependency: the GUI uses it to enumerate devices and move
applications between them.

Two things worth knowing when moving between machines:

- **Presets are machine-specific.** They store device node names, so a preset
  from another PC will come up with unassigned inputs and outputs. The engine
  falls back to matching a device by its description, which covers the same
  hardware on a different USB port, but not different hardware.
- **The Hyprland keybindings are not installed for you.** `install.sh` never
  touches another app's config. `packaging/betterbanana.lua.example` is a
  ready-made copy — see [Keyboard shortcuts](#keyboard-shortcuts).

## Install

    make && make gui && make install

`make install` puts the three binaries in `~/.local/bin`, registers an icon and
desktop entry, and enables a **systemd user service** so the engine is already
running when you log in:

    systemctl --user status betterbanana-engine

It restarts the service on every install, since replacing a binary in place
leaves a running process on the old inode.

## Build

    make            # engine + bb-ctl + dsp tests
    make gui        # Qt6 GUI

No cmake needed; Qt's `moc` is invoked directly (Qt6's, at `/usr/lib/qt6/moc` —
`/usr/bin/moc` is Qt5's on Arch). Requires `libpipewire-0.3`, `Qt6Widgets`,
`sndfile`.

## Run

    ./build/bb-engine &     # creates the virtual devices, must run first
    ./build/bb-gui

Only one engine may run at a time; a second instance refuses to start rather
than publishing duplicate node names and fighting over the graph.

The engine registers four devices any application can use:

| Device      | Kind          | Purpose                                  |
|-------------|---------------|------------------------------------------|
| `bb_vaio`  | Audio/Sink    | apps play *into* it → strip "VAIO"       |
| `bb_aux`   | Audio/Sink    | apps play *into* it → strip "AUX"        |
| `bb_b1`    | Audio/Source  | apps record *from* it ← bus B1           |
| `bb_b2`    | Audio/Source  | apps record *from* it ← bus B2           |
| `bb_cable1..3` | Audio/Sink | virtual cables, assignable to any strip  |

Hardware inputs 1–3 and buses A1–A3 are idle until you assign a device.

## Themes

**Theme** menu, remembered across runs: BetterBanana Dark, Catppuccin Mocha,
Catppuccin Latte, Everforest Dark, Nord, Gruvbox Dark, Tokyo Night, Dracula,
Rosé Pine and Solarized Dark.

Themes are data, not stylesheets — see `builtinThemes()` in `gui/theme.cpp`.
Add one by appending a `mk(...)` row; every widget, including the custom-painted
knobs, Intellipan pads and meters, follows automatically.

## Tape deck

Records any bus to a 24-bit WAV, and plays a file back *into* the matrix through
its own bus-assign row, so bus EQ and gain apply to it. File I/O runs on helper
threads feeding lock-free rings; the realtime mixer never touches the disk.
Playback resamples (linearly) when the file's rate differs from the graph's.

## VBAN

**Engine → VBAN streams…** (or `Ctrl+B`, or `bb-gui --vban`) configures 8
outgoing and 8 incoming streams, implemented with PipeWire's native
`vban-send` / `vban-recv` modules.

- Each **outgoing** stream transmits one bus to a host:port.
- Each **incoming** stream appears as a normal source named `bb_vban_in_N`,
  so you select it on a hardware strip like any other device.

## Renaming strips and buses

**Right-click any strip or bus title plate** for *Rename…* / *Reset to default*.
Names are stored in the shared state and saved with presets, so they travel with
the rest of the configuration. From the shell:

    ./build/bb-ctl label strip 0 "MIC - SM7B"
    ./build/bb-ctl label bus A1 "SPEAKERS"

## Parametric EQ

Every input strip and every bus has its own twelve-band parametric EQ. The **EQ**
button bypasses the whole block; **right-click the EQ button** (or use
**Engine → Input EQ** / **Engine → Bus EQ**) to open the editor.

The two are the same editor on the same kind of block, so a curve, a profile or
an AutoEq import works identically wherever you use it. What differs is what
they are *for*: a bus EQ corrects what you are listening on, and a strip EQ fixes
what is coming in — a high pass under desk rumble, a narrow notch on a fan or a
room mode, a dip where a microphone is harsh.

Every band picks its own shape — **peak, low shelf, high shelf, high pass, low
pass, notch, band pass** — and carries gain, frequency, Q and its own bypass.
Above them sits a **preamp**, so a curve that boosts can be pulled back to where
it cannot clip; **Auto** sets it to exactly clear the highest peak.

Two ways to edit, on the same state:

- **The curve.** Drag a numbered handle for frequency and gain, wheel over it
  for Q, right-click to bypass that band, double-click to zero its gain. Each
  band's own response is drawn faintly behind the summed curve.
- **The table.** Type exact numbers. Frequency and Q step logarithmically, so
  one notch is a semitone at 40 Hz and at 12 kHz.

The curve is computed with the engine's own `Biquad`, so it shows what you
actually hear, preamp included.

**Undo** (or `Ctrl+Z`) steps back through the edits made in the dialog. A whole
drag or wheel spin is one step, not thirty.

### The spectrum

Behind the curve is the engine's live analysis of the signal this EQ sits in —
a strip's EQ shows the strip post-processing, a bus's shows the bus output — so
a boom or a whistle can be seen rather than guessed at. The shaded area is read
against the **dBFS scale down the right-hand edge**; the numbers on the left are
EQ gain, as before.

The two scales are deliberately aligned: 90 dB of level spans the same height as
36 dB of EQ, which puts every gridline on a round level (+18 dB is 0 dBFS, 0 dB
is −45 dBFS, −18 dB is −90 dBFS).

Analysis runs on the engine's control thread, never on the audio thread, and
only while an editor is open — the mixer does no FFT work when nobody is
looking. One signal is analysed at a time, which is all anyone can read.

Bands that are bypassed — or left as a flat peak — are dropped from the audio
path entirely, so a twelve-band EQ using three bands costs three biquads.

The per-strip **LOW / MID / HIGH** knobs are a separate, always-active tone
control at fixed frequencies (100 Hz shelf, 1 kHz peak, 8 kHz shelf). They sit
*before* the parametric block and need no enabling; think of them as the quick
shape and the parametric as the surgical one.

### EQ profiles

The **PROFILE** row saves and recalls just the EQ, separately from mixer
presets. Twelve built-ins ship with it — Bass Boost, Loudness, Vocal Clarity,
Speech / Podcast, Gaming (footsteps), De-harsh, Warm, Small Speakers and the
rest — each with a preamp that keeps it clipping-safe.

**Save as…** writes your own to `~/.config/betterbanana/eq/<name>.txt`, and
**Import…** / **Export…** read and write the same files. That format *is*
[Equalizer APO](https://sourceforge.net/projects/equalizerapo/)'s parametric
export — the format Peace uses on Windows and AutoEq publishes — so profiles
move between BetterBanana, Peace and anything else that speaks it, unchanged:

    Preamp: -6.1 dB
    Filter 1: ON LSC Fc 105 Hz Gain 6.4 dB Q 0.70
    Filter 2: ON PK Fc 8800 Hz Gain 5.1 dB Q 1.42

A profile with more than twelve filters keeps the ones that shape the sound most
— shelves and pass filters always, then the largest boosts and cuts.

### Headphone EQ (AutoEq)

**Headphone EQ…** in the EQ editor searches the
[AutoEq](https://github.com/jaakkopasanen/AutoEq) database: measured corrections
for roughly 8850 headphones and IEMs, from oratory1990, crinacle, Rtings,
Innerfidelity and two dozen other measurers. Type a model, pick a measurement,
and its parametric EQ — preamp included — lands on the bus.

This is the equivalent of what Peace's database gives you on Windows. The index
is cached in `~/.config/betterbanana/autoeq/`, so searching is instant and works
offline; only downloading a profile needs the network. Every profile you apply
is also saved into your own EQ directory, so it stays selectable afterwards.

**If you switch between several pairs of headphones**, tick *"Re-apply
automatically whenever this bus is set to …"*. The profile is then remembered
against that **output device**, not the bus: point the bus at a different pair
and its correction follows automatically.

From the shell, `bb-autoeq` does the same thing and is easy to bind to a key:

    bb-autoeq search hd 650             # what is available
    bb-autoeq apply A2 hd 650           # download and apply to bus A2
    bb-autoeq update                    # refresh the cached index
    bb-autoeq list                      # profiles saved on this machine

It downloads the profile and hands it to `bb-ctl eq load`, so the parsing, the
band fitting and the preamp are the mixer's own code.

## Voice changer

Every input strip has one, behind the **FX** button next to its EQ — left-click
to bypass, right-click to edit, or **Engine → Voice changer**. It sits *after*
that strip's EQ and *before* its fader, so the EQ cleans the real voice going in
rather than the artefacts coming out, and the fader still means level.

| Control | What it does |
|---|---|
| **PITCH** | ±12 semitones. Zero is a true bypass. |
| **FORMANT** + **SEPARATE** | ±12 semitones of vocal-tract resonance, moved independently of pitch |
| **DRIVE** | tanh saturation, normalised so it changes shape rather than level |
| **RING** / **RING MIX** | ring modulator — the robot voice. 0 Hz is off. |
| **BITS** / **HOLD** | bit depth and sample-and-hold, separately |
| **CHORUS** / **CH RATE** / **CH MIX** | modulated delay, for thickening or detune |
| **ECHO** / **FEEDBACK** / **ECHO MIX** | up to 1 s |
| **GAIN** | makeup, because most of the above change the level |

Fourteen presets ship with it. **Feminine, Masculine, Higher** and **Deeper**
move the formants separately; **Chipmunk, Squeaky, Deep, Demon, Robot, Alien,
Lo-fi, Cave** and **Detuned** deliberately do not. The combo drops to
*(custom)* the moment you move a knob off one. Everything a preset does is reachable by hand; they exist so the
first thing you do is not stare at twelve knobs.

    bb-ctl fx list                          # what is available
    bb-ctl strip 0 fx preset "Demon"
    bb-ctl strip 0 fx pitch -5              # or set it by hand
    bb-ctl strip 0 fx echo 160 0.45 0.4
    bb-ctl strip 0 fx show
    bb-ctl strip 0 fx off

**A telephone or radio voice is an EQ recipe, not an effect** — a high pass at
300 Hz and a low pass at 3.4 kHz on the strip's own EQ is the whole trick. No
preset touches your EQ, because silently rewriting a curve you tuned would be a
nasty surprise.

### Sounding like a different person, not a different size

Two things decide whether a voice reads as female or male, and they are
independent:

- **Pitch.** Typical adult male speech sits around 110–130 Hz, female around
  190–220 Hz — six to nine semitones apart.
- **Formants**, the resonances of your vocal tract. A shorter tract puts them
  perhaps 15–20% higher. This is the one a listener hears as *body size*.

The pitch shifter is a resampler with the duration patched up, so on its own it
moves **both** — that is tape speed, and it is why a large shift sounds like a
small person rather than a different one. Turn on **SEPARATE** and the FORMANT
knob becomes the net formant shift, whatever pitch is doing: the stage behind it
only has to make up the difference, so both numbers read as absolutes.

Leaving FORMANT at 0 with SEPARATE on and PITCH up is therefore *not* a no-op —
it pulls the formants back down to where they started, giving a high voice from
an unchanged body.

Formant shifting is a short-time Fourier transform: each frame's magnitude is
split into a smooth envelope (the formants) and everything else (the harmonics,
which carry pitch), the envelope is stretched along the frequency axis, and it
is put back with the phase untouched. Not touching the phase is why it avoids
the smeared quality a full phase vocoder has. It costs another ~20 ms of latency
and about 4–5% of a core, and it runs only when SEPARATE is on.

`tests/test_voicefx.cpp` checks the property that matters: after a shift the
envelope has moved by the requested ratio, while the harmonic comb still stands
50× clear of the gaps between its teeth — the envelope moved and the pitch did
not.

### Calibrating it to your own voice

**Calibrate to my voice…** in the voice changer records a few seconds, measures
where your voice actually sits, and works the shift out from there. A preset
cannot do this: lifting six semitones lands a 95 Hz voice at 134 Hz and a 140 Hz
voice at 198 Hz — the same setting, two completely different results.

1. Read the phrase aloud for eight seconds.
2. It reports your median pitch, the range around it, and how much of the
   recording was actually voiced.
3. Set a **target** — adult female speech typically sits around 190–220 Hz — and
   the pitch shift follows from it.
4. **Play what I said** and **Play it shifted** run your own recording through
   the real DSP, so you judge it by ear rather than by number. Adjust, replay.
5. **Apply to this strip.**

It is a measurement, not a model: it runs offline in a second, needs no network,
no training and no GPU, and every number it picks is visible and editable
afterwards. Nothing is recorded until you press the button, the recording never
leaves the machine, and it is deleted when the dialog closes.

Pitch is measured by normalised square difference (`engine/pitchtrack.h`),
picking the lowest strong lag rather than the largest peak — the classic failure
of autocorrelation on a voice is locking onto twice the true period and
reporting an octave low. `tests/test_pitch.cpp` checks it against synthetic
voices from 85 to 300 Hz and confirms it stays quiet on noise and silence.

**Formants are the part you have to judge by ear.** Pitch measures reliably;
picking formants off a live microphone does not, because the room and the mic
colour the spectrum as much as the speaker does. So the wizard measures pitch,
starts formants at a sensible +3 semitones, and expects you to try a semitone
either side. That honesty is the reason the playback loop matters more than the
cleverness of the analysis.

### How the pitch shifter works, and what it costs

It is a delay-line shifter: one read head sweeps through a buffer at the pitch
ratio, and a second head takes over across a short crossfade each time the first
runs out of window. No FFT on the audio thread, no allocation, no dependency.

Two consequences worth knowing:

- **It adds about 20 ms**, on top of the engine's own buffering. Your listeners
  will not notice — they are further away than that already — but *you* hearing
  yourself through it will. Use your interface's direct monitoring for your own
  ears and take the strip off the bus feeding your headphones, so the effect
  only sits on the path going out.
- **The seam is locked to your pitch.** The head splices back by one sweep
  whenever it runs out of window, and if that sweep is an arbitrary length the
  waveform does not line up either side of the splice. The mismatch then repeats
  at the sweep rate — 15 to 20 Hz for a large shift, which is exactly where the
  ear hears roughness. It sounds robotic.

  So the sweep is set to a whole number of the speaker's own pitch periods,
  tracked by autocorrelation and refined to a fraction of a sample. Rounding
  that period to whole samples is not good enough: four samples of error per
  sweep puts the tenth harmonic sixty degrees out at the seam, and on its own
  that held the modulation at 2.4%.

  The crossfade law changes with it. Aligning the taps makes them read
  near-identical signal, and summing correlated signals with the constant-*power*
  law puts +3 dB in the middle of every seam — measurably worse than the buzz
  the alignment just removed. Correlated wants constant amplitude; unvoiced
  audio, where no period is found, still wants constant power. It picks per
  seam.

  Measured on a 115 Hz voice: **5.5% amplitude modulation before, 0.2% after**,
  which is the test signal's own noise floor. The tracker costs nothing
  measurable — a pitch-shifting strip reads the same DSP load as an idle one —
  and it runs only while the pitch stage does.

  The crossfade is also short rather than the more obvious half-window overlap:
  two heads summed the whole time sit a fixed distance apart and cancel each
  other wherever that distance is a half period, which sounds hollow.
  `tests/test_voicefx.cpp` measures all of it — the shifted tone is the dominant
  partial, and the modulation added stays under 1%.

RubberBand would do the pitch side better than this, but it is GPL and this is
MIT; the formant stage above is what actually mattered, and it needed no
dependency at all.

## Analysing a microphone

**Engine → Analyse microphone** lists every hardware strip with a real capture
device, plus the two virtual output buses, and opens
[mic-gain](../mic-gain/) on the one you pick. That tool measures the signal and
names the control to change — gain, high-pass, de-esser, compressor — rather
than leaving you to guess by ear.

Pointing it at **B1 or B2** rather than at the interface is usually the more
useful measurement: those are what recording applications actually receive, so
the reading includes this mixer's gate, compressor, EQ and fader.

mic-gain is a separate tool on purpose — it is a terminal program, so it still
works over SSH and when this GUI will not start, which is exactly when a
microphone needs diagnosing. It shares no code with the engine; the two are
joined only by this menu.

Installing BetterBanana from a clone of the repository installs it too, since it
is sitting right there and needs nothing BetterBanana does not already require
(`python3`, for the watchdog and the stream guard). Installing from a source
tarball does not, because the tarball contains BetterBanana alone — the
installer says so, and the menu says so if it cannot find it. Either way,
`mic-gain/install.sh` installs it on its own.

Strips fed by a virtual cable, or with no device assigned, are listed but greyed
out: there is no microphone there to measure.

## Sidechain ducking

**Engine → Sidechain ducking…** (`Ctrl+D`). Mark the strips that should *trigger*
ducking as **KEY** (usually your microphone), and give the strips that should
*step back* a **DEPTH** in dB. While a key strip is above the threshold, every
strip with a depth is pulled down by that amount, then released smoothly.

The key level is taken after that strip's gate and compressor, so the gate
decides what counts as speech rather than every keyboard click.

    bb-ctl strip 0 key 1        # microphone triggers ducking
    bb-ctl strip 1 duck -12     # this strip drops 12 dB while you talk
    bb-ctl duck on

## Start at login

**Engine → Start at login** has two independent toggles:

- **Audio engine** — enables/disables the `betterbanana-engine` systemd user
  service. Leave this on: with the engine stopped, the virtual devices do not
  exist, so anything pointed at them falls back to your default output.
- **Mixer window** — whether the GUI opens automatically, via an XDG autostart
  entry at `~/.config/autostart/betterbanana.desktop`.

The installer turns the engine on and leaves the window off. Both reflect
changes made outside the app, and there is a shell equivalent:

    bb-ctl autostart                 # show what starts at login
    bb-ctl autostart engine off
    bb-ctl autostart gui on

## The watchdog

The engine's worst failure is not a crash. When the PipeWire graph is torn down
underneath it, the engine keeps running — same PID, heartbeat still ticking — so
`bb-ctl status` answers, `systemctl` reports the unit active, and every check you
would think to make says the mixer is fine. Its nodes are simply gone from the
graph. No audio moves and nothing notices.

`bb-health` watches the only thing that settles it: whether the engine's nodes
are actually in the graph. When they are not, it restarts the engine and puts
your configuration back.

    systemctl --user status betterbanana-health
    journalctl --user -u betterbanana-health -f

It keeps its own config snapshot (`~/.config/betterbanana/lastgood.bbp`), taken
whenever the mixer looks healthy. That matters because a restarted engine loads
your **startup preset** — a deliberate choice you made once, not necessarily what
you have been doing since. Your default sink and source are recorded alongside it,
because a PipeWire restart resets those and they are normally `bb_vaio` and
`bb_b1`; losing them quietly sends every application to the wrong device.

Repairs are rate-limited and give up after a few consecutive failures, so an
engine that is genuinely broken produces a loud log instead of a restart loop.
Snapshots are only taken while the mixer is healthy, so a bad state never
overwrites a good one.

To save or restore config yourself at any time:

    bb-ctl preset save <name|path>
    bb-ctl preset load <name|path>
    bb-ctl preset list

## If PipeWire restarts

Restarting PipeWire — or a package update doing it for you — destroys every node
in the graph, the engine's included. The engine does not reconnect, and it does
not exit either: it keeps running with its heartbeat ticking, so `bb-ctl status`
still answers and the service still reads `active` while the mixer has silently
disappeared from the graph. Nothing looks wrong except that no audio moves.

The shipped unit sets `PartOf=pipewire.service`, so systemd restarts the engine
whenever PipeWire restarts and the nodes come back on their own. If you are
running an older install, or the engine ends up in this state anyway:

    systemctl --user restart betterbanana-engine

A PipeWire restart also resets things outside BetterBanana that it cannot restore
for you — card profiles, and your default sink and source:

    pactl set-card-profile <card> <profile>
    pactl set-default-sink bb_vaio
    pactl set-default-source bb_b1

Bluetooth headsets are worth watching here. They offer either high-quality
stereo playback (A2DP) or the mono headset profile with a microphone, never both
at once, and everything you hear through the headset profile is tinny and mono.

WirePlumber switches to that profile automatically whenever an application starts
recording — and the engine is always recording, so a mixer plus Bluetooth
headphones means the switch fires constantly. If your microphone is a separate
interface and you never want the headset's mic, turn it off once:

    wpctl settings --save bluetooth.autoswitch-to-headset-profile false

Assigning the headset's own microphone to a strip has the same effect and will
override this, so leave that strip unassigned unless you really want the mono
profile.

After a PipeWire restart the headset often reconnects offering only HSP/HFP —
A2DP is not in the profile list at all, so setting the profile fails with "No
such entity". Renegotiate it with a full reconnect, then point the bus back at
it:

    bluetoothctl disconnect <mac> && sleep 4 && bluetoothctl connect <mac>
    bb-ctl route out A2 bluez_output.<mac>.1

## Keyboard shortcuts

`packaging/betterbanana.lua.example` binds the mixer to Hyprland. It is *not*
installed by `install.sh`; drop it in yourself:

    cp packaging/betterbanana.lua.example ~/.config/hypr/config/betterbanana.lua
    echo 'require("config.betterbanana")' >> ~/.config/hypr/hyprland.lua

That config is Lua-based, not hyprlang, so the binds use `hl.bind` and
`hl.dsp.exec_cmd`. On a hyprlang setup, translate them into `bind = ` lines.

| Key | Action |
|---|---|
| `SUPER + M` | mute / unmute the microphone strip |
| `SUPER + SHIFT + M` | toggle sidechain ducking |
| `SUPER + SHIFT + V` | open the mixer |

They call `bb-ctl ... toggle`, which flips the current value and prints the new
one. Delete that file and its `require()` line in `hyprland.lua` to remove them.

## DSP load

The status bar and `bb-ctl status` show what fraction of its realtime deadline
the mixer is actually using — 100% would mean it took exactly as long to compute
a block as the block lasts, and anything near that drops out. It rises instantly
and falls about 1.5% per block, so a brief spike stays visible long enough to
read.

Idle sits under 1%. A strip with the voice changer's formant shifting on costs
roughly 2.5% on top, since that is the one part of the chain doing an FFT.

## Metering

Each strip shows gain reduction the engine was already computing: a bar under
**GATE** for gate attenuation, under **COMP** for compression, and one below the
gain readout for the ducker. Meters latch a **clip indicator** across the top
when a signal hits full scale — click any meter, or use **Engine → Clear clip
indicators**, to reset them. Meters carry a faint dB ruler at -6, -12, -20, -30
and -40, with unity marked more brightly.

**Solo is per bus.** A soloed strip silences the others only on the buses it
actually feeds, so soloing something that feeds B1 leaves B2 alone.

## Faders

Custom-drawn, linear in dB, with a marked unity position and a dB scale.
**Double-click a fader to snap it back to 0 dB.** Drag to set, hold Ctrl while
dragging for fine control, and the wheel works once you have clicked the fader.

## A note on the mouse wheel

Scrolling over a device dropdown or a fader does nothing. Qt normally lets the
wheel change those without focus, which meant a stray scroll over the window
could silently re-route an application or nudge a bus gain. Faders respond to
the wheel only after you click them; device pickers never do. The knobs are
deliberate controls and still take the wheel on hover (hold Ctrl for fine steps).

## Presets

State is saved as plain text (`common/preset.h`): every strip, bus, device
assignment, recorder setting and VBAN stream.

Presets are **explicit**. The mixer does not save your session when you quit and
does not silently reload it — it loads exactly the one preset you nominate, and
nothing else.

- **Preset menu** — Save (`Ctrl+S`), Load (`Ctrl+O`), and **Load on startup**.
- **Load on startup** lists your saved presets; the one you tick is what the
  engine restores when it starts. `(none)` means it comes up with a default
  mixer. The choice is recorded in `~/.config/betterbanana/startup`.
- Named presets live in `~/.config/betterbanana/presets/`.

        ./build/bb-ctl preset save studio
        ./build/bb-ctl preset load studio
        ./build/bb-ctl preset list
        ./build/bb-ctl preset startup studio    # load "studio" at engine start
        ./build/bb-ctl preset startup           # show the current choice
        ./build/bb-ctl preset startup none      # back to a default mixer

Preset files are written through a temporary and renamed into place, so a crash
or a full disk cannot leave a half-written preset that comes back as a
half-configured mixer.

Loading a preset whose devices are not plugged in says so, and names them,
rather than leaving those strips quietly unconnected.

**Upgrading from an older version:** the engine used to save your session to
`autosave.bbp` on exit and reload it on start. The first time the new engine
runs it turns that file into an ordinary preset called *Previous session* and
makes it the startup choice, so nothing is lost and nothing changes underfoot.

## Undo

`Ctrl+Z` and `Ctrl+Shift+Z` (**Edit** menu) step the whole mixer backwards and
forwards — faders, routing, EQ, ducking, everything a preset covers.

It works by watching the shared state rather than by instrumenting each control,
which has two consequences worth knowing. A change made from `bb-ctl` in another
terminal is undoable too. And a change is recorded once it has *stopped* moving,
so a fader sweep or an EQ drag is one step rather than thirty.

## Settings that follow a microphone

**Right-click a hardware strip's device picker** to remember that strip's
processing for whatever is selected — gate, compressor, EQ, level and pan. Point
any strip at that device later and the settings come back.

Bus assignment is deliberately *not* included: which buses a strip feeds belongs
to the mix, not to what is plugged in.

Nothing is stored unless you ask for it, and *Forget them* removes it. Snapshots
live in `~/.config/betterbanana/devices/`.

This is the input-side twin of the AutoEq *"re-apply automatically for this
device"* option, which does the same thing for headphone corrections on a bus.

## Virtual cables

Three standalone cables (`bb_cable1..3`, shown as *BetterBanana Cable 1–3*) act
like VB-Audio's Virtual Cable: each is a sink any application can play into, and
each can be assigned as the source of **any hardware input strip** from that
strip's device dropdown.

That gives a dedicated strip — its own fader, EQ, gate and bus routing — to
whichever applications you point at that cable, separately from VAIO and AUX.
For example, put *BetterBanana Cable 1* on **HARDWARE INPUT 2**, then send a game
or a browser to it while music keeps going through VAIO.

Cables feed their strip inside the engine rather than through a monitor source,
so there is no extra latency or conversion. A cable feeds at most one strip; the
routing value is stored as `cable:N`.

## Routing applications (the virtual-cable workflow)

**Engine → Applications…** (`Ctrl+A`, or `bb-gui --apps`) lists everything
currently playing or recording and lets you point each one at a BetterBanana
endpoint. WirePlumber remembers the choice, so it survives the app restarting.

Device lists show PipeWire's friendly `description` ("UMC202HD 192k Line A")
rather than the node name, with the full name on hover; the node name is still
what gets stored, so presets stay stable.

Only applications that are **currently producing audio** own a stream, so that
is all the system can see — Discord, for instance, appears the moment it plays
or opens its microphone. Choosing a target therefore also **saves a rule** for
that application, listed under *REMEMBERED*: the next time it starts, it is
re-routed automatically. Rules apply whenever the GUI is running, not only while
the dialog is open, and are applied once per stream so a later manual move is
respected.

To send music into your microphone the way BetterBanana does:

1. In **Applications**, set Spotify's target to **→ BetterBanana VAIO**.
2. On the **BETTERBANANA VAIO** strip, enable **A1** (so you still hear it) and
   **B1** (so it reaches the virtual mic).
3. Assign a real output device to bus **A1**.
4. Put your microphone on **HARDWARE INPUT 1** and enable **B1** only — leaving
   A1 off is what stops you hearing yourself and prevents a feedback loop.
5. In Discord, choose **`bb_b1`** as the input device.

Discord then hears your microphone and Spotify mixed, each with its own fader.

### Adding Discord voice on AUX

Set Discord's **output** to *BetterBanana AUX* so incoming voice gets its own
strip, and enable **A1 only** on it.

**AUX must not have B1 enabled.** B1 is what Discord transmits, so routing
Discord's own output back into it sends everyone their own voice as an echo.
The same rule applies to any strip carrying an application's output that is
also listening on B1.

The resulting layout:

| Strip | Source | A1 (speakers) | B1 (to Discord) |
|---|---|:--:|:--:|
| HARDWARE INPUT 1 | microphone | — | yes |
| HARDWARE INPUT 2 | BetterBanana Cable 1 (game / apps) | yes | yes |
| BETTERBANANA VAIO | Spotify | yes | yes |
| BETTERBANANA AUX | Discord voice | yes | **no** |

Discord: output = *BetterBanana AUX*, input = *BetterBanana Out B1*.

Note the two named from the mixer's point of view: VAIO and AUX are inputs *to
the mixer*, so applications list them as **playback** devices. The only things
you record *from* are `bb_b1` and `bb_b2`.

### Screen sharing on Discord

Sharing your screen while running a mixer breaks in two ways that are invisible
from Discord's UI. `bb-stream-guard` — installed and enabled for you — fixes
both, but it is worth knowing what it is doing.

**Everyone hears themselves.** Discord's screen-share capture auto-links to
every `bb_a*` output bus it can find. Your monitoring buses carry the AUX strip,
so the voices of the people you are talking to get folded straight back into
your stream and each of them hears their own echo.

**Audio that is routed perfectly is still silent.** Discord runs *several* nodes
all named `discord_capture` — four is normal — and only one is actually
transmitted to viewers. `pw-link` matches ports by name, so linking a bus to
"discord_capture" hits an instance at random. This is the one that wastes an
evening: the routing is correct, the meters move, and your friends hear nothing.

The fix is a dedicated stream bus that carries only what you want streamed, wired
to every capture instance by port id. One command sets that up:

    bb-stream-setup

It claims a free output bus, points it at the `betterbanana_stream` null sink, and
routes your application audio — the VAIO strip and anything fed by a virtual
cable — to it. Microphones are left out on purpose: Discord already sends your
voice, so streaming it as well means viewers hear you twice. AUX is never
routed, and if it already was, the tool removes it.

It is safe to re-run, it never takes a bus that already has a device, and
`--dry-run` shows what it would do without touching anything. To do it by hand
instead:

    bb-ctl route out A3 betterbanana_stream     # A3 becomes the stream bus
    bb-ctl strip 1 bus A3 1           # game / app audio -> stream
    bb-ctl strip 3 bus A3 1           # music -> stream

**Never route AUX to the stream bus.** That is the echo, straight back.

`bb-stream-setup` will refuse to run until `betterbanana_stream` exists. That
sink comes from `99-bb-stream.conf`, and PipeWire only reads its configuration at
startup — so on a fresh install, log out and back in first.

| Bus | Carries | Who hears it |
|---|---|---|
| A1 / A2 | everything, AUX included | you |
| A3 | apps and music, no AUX | your viewers |

The guard discovers the stream bus rather than assuming it: whichever bus you
have assigned to `betterbanana_stream` is the one it feeds to Discord, and every other bus
is kept out. If you never set one up it still runs, and still stops the buses
carrying AUX from reaching Discord — so the echo cannot happen either way.

    systemctl --user status betterbanana-stream-guard
    journalctl --user -u betterbanana-stream-guard -f

Two things that look like routing faults and are not:

- **An app that goes quiet is often just paused.** Check `Corked:` in
  `pactl list sink-inputs`. Games under Wine open a new stream per sound and cork
  between them, so `-99.9 dB` on a strip can simply mean nothing is playing.
- **The stream bus can vanish.** A null sink with nothing playing gets suspended
  as idle, and that takes BetterBanana's bus node down with it — the stream then
  goes silent until something restarts the chain. The shipped
  `99-bb-stream.conf` disables suspend for `betterbanana_stream`, and the guard re-creates
  the bus if it disappears anyway.

## Troubleshooting

### An app plays into the wrong device, and changing its setting does nothing

Two separate things decide where an application's audio goes: the device picked
inside the app, and the device WirePlumber remembers for that app. **The
remembered one wins.** If they disagree, the app asks politely for the right
sink on every new stream and is dragged back to the old one — so changing the
setting inside the app appears to do nothing at all, and the change seems to
"not stick" across restarts.

This is worth recognising quickly, because it looks exactly like a mixer fault:
the strip is configured correctly, unmuted, and reads nothing.

Compare what the app asked for against where it ended up:

    pactl list sink-inputs | grep -E 'Sink:|target.object|application.name'

`target.object` is the app's request; `Sink:` is where it actually is. Map the
sink number with `pactl list sinks short`. If the two disagree, WirePlumber's
saved state is overriding the app.

The fix is to move the stream **while it is playing** — that updates the app and
WirePlumber's memory together, where changing the setting in the app only does
the first:

    pactl move-sink-input <id> bb_aux

Any volume mixer's "move to another device" does the same thing. WirePlumber
persists the new target within a few seconds; its state lives in
`~/.local/state/wireplumber/stream-properties`, but editing that by hand is
pointless while WirePlumber is running — it rewrites the file itself.

### Discord's voice is inaudible

Usually the above, with Discord's output landing on the stream sink instead of
`bb_aux`. Audio played into `betterbanana_stream` is heard only by the people
watching your screen share, never by you — and if it is Discord's own output,
everyone in the call hears themselves. Point Discord's output at **BetterBanana
AUX**, and check with the `pactl` command above that it actually went there.

### A newly installed sink or source does not exist

PipeWire reads `~/.config/pipewire/pipewire.conf.d/*.conf` only at startup. A
freshly installed `99-bb-stream.conf` has no effect until you log out and back in.

## Control from the shell

    ./build/bb-ctl status
    ./build/bb-ctl meters 20
    ./build/bb-ctl strip 3 gain -6
    ./build/bb-ctl route out A1 alsa_output.usb-...-00.HiFi__Line__sink
    ./build/bb-ctl rec bus B1 && ./build/bb-ctl rec file take.wav && ./build/bb-ctl rec start
    ./build/bb-ctl vban out 1 host 192.168.1.20 && ./build/bb-ctl vban out 1 on
    ./build/bb-ctl vban apply

    ./build/bb-ctl eq list                      # built-in and saved profiles
    ./build/bb-ctl eq load A1 "Bass Boost"      # a built-in, a saved one, or a file
    ./build/bb-ctl eq load A2 ~/Downloads/HD650.txt
    ./build/bb-ctl eq save A1 "speakers"        # keep the current curve
    ./build/bb-ctl eq show A1                   # print it as Equalizer APO text
    ./build/bb-ctl eq preamp A1                 # set a clipping-safe preamp
    ./build/bb-ctl bus A1 band 0 6.0 105 0.7 ls # gain, freq, Q, shape

Every `eq` subcommand takes a **bus** (`A1`–`A3`, `B1`, `B2`) or an **input
strip** (`s0`–`s4`), because they are the same kind of block:

    ./build/bb-ctl eq load s0 "Speech / Podcast"   # onto hardware input 1
    ./build/bb-ctl eq show s0
    ./build/bb-ctl strip 0 eqon 1                  # enable that strip's EQ
    ./build/bb-ctl strip 0 band 0 -4.0 80 0.7 hp   # 80 Hz high pass on the mic

## Tests

    make check                # every unit test, naming whichever one broke

    ./build/test_dsp          # 30 DSP assertions, no audio server needed
    ./build/test_eq           # 45 EQ profile / import / preset assertions
    ./build/test_preset       # 53 preset, startup and per-device assertions
    ./build/test_spectrum     # 14 FFT and analyser-calibration assertions
    ./build/test_voicefx      # 36 voice changer assertions, measured by FFT
    ./build/test_pitch        # 22 pitch detection assertions, 85-300 Hz
    ./build/test_fader        # fader ballistics
    ./tests/integration.sh    # drives real audio through a running engine

`tests/integration.sh` needs a running engine and drives real audio through it,
so it is deliberately not part of `make check`. It parks any application sitting
on a BetterBanana sink for the duration and puts each one back on the sink it
came from.

`test_preset` checks that serialise → deserialise → serialise is byte-identical
on a fully populated mixer. Undo is built out of those two functions, so a field
the format quietly dropped would be a control `Ctrl+Z` silently failed to
restore.

## Design

All PipeWire nodes live in one process sharing `node.group="betterbanana"`, so the
graph schedules them under a single driver — verified in
`tests/probe_stream.cpp`, which also established that `pw_stream` (not
`pw_filter`) is required: only `pw_stream` wraps the node in an adapter that
pipewire-pulse will expose as a real device.

Input endpoints push into per-strip rings; output endpoints pull from per-bus
rings and run the mixer on demand. Every endpoint runs on the same data-loop
thread, so the mixer needs no locking. The tape deck and each VBAN sender get
their own ring rather than sharing a bus ring, which is single-consumer. All
rings are bounded, so an idle or unassigned consumer cannot accumulate latency.

The segment is never `shm_unlink`ed, so its inode is reused: a running GUI
keeps working across an engine restart instead of silently holding a dead
mapping.

The GUI and engine share one POSIX shared-memory segment (`/betterbanana.state`):
continuous parameters are plain atomics, while strings (device assignments,
file paths, VBAN config) sit behind seqlocks and are applied off the realtime
thread. The segment carries both a version and `sizeof(Shared)`, so mismatched
binaries refuse to talk instead of silently writing to the wrong offsets.

## Status

Working and verified: virtual devices, routing matrix, per-strip gate /
compressor / tone knobs / audibility / Intellipan / mono / solo / mute / fader,
the 12-band parametric EQ on every strip and bus with profiles, AutoEq import
and a live spectrum analyser, the per-strip voice changer with independent
pitch and formant shifting, undo across the whole
mixer, per-bus mono / mute / fader, metering, hardware assignment, the tape deck,
and VBAN send/receive.

Not implemented: the surround bus modes, which need buses wider than stereo.
