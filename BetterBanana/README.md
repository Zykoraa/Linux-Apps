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
- 6-band parametric EQ per bus with a live response curve
- Per-application routing that survives the app restarting
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
- **The Hyprland keybindings are not part of the package.** Copy
  `packaging/`-style binds yourself, or lift
  `~/.config/hypr/config/betterbanana.lua` across.

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

## Bus EQ

Each bus has a six-band parametric EQ. The **EQ** button bypasses it;
**right-click the EQ button** (or use **Engine → Bus EQ**) to open the editor:
gain, frequency and Q per band over a live response curve. The curve is computed
with the engine's own `Biquad`, so it shows what you actually hear. Frequency
knobs are log-scaled across 20 Hz – 20 kHz.

The per-strip **LOW / MID / HIGH** knobs are a separate, always-active 3-band EQ
and need no enabling.

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
at once. If anything claims the headset's mic — including assigning it to a
strip — the device drops to the headset profile and everything you hear through
it turns tinny and mono. Set your defaults back first, then reconnect it:

    bluetoothctl disconnect <mac> && sleep 4 && bluetoothctl connect <mac>

## Keyboard shortcuts

`packaging/` is wired into Hyprland via `~/.config/hypr/config/betterbanana.lua`:

| Key | Action |
|---|---|
| `SUPER + M` | mute / unmute the microphone strip |
| `SUPER + SHIFT + M` | toggle sidechain ducking |
| `SUPER + SHIFT + V` | open the mixer |

They call `bb-ctl ... toggle`, which flips the current value and prints the new
one. Delete that file and its `require()` line in `hyprland.lua` to remove them.

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

- **Preset menu** — Save (`Ctrl+S`), Load (`Ctrl+O`), or *Save as default*.
- The engine **restores `~/.config/betterbanana/autosave.bbp` on start and rewrites
  it on exit**, so it reopens where you left off.
- Named presets live in `~/.config/betterbanana/presets/`.

        ./build/bb-ctl preset save studio
        ./build/bb-ctl preset load studio
        ./build/bb-ctl preset list

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

The resulting layout, which is what the shipped `spotify-to-mic` preset holds:

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
to every capture instance by port id. Point a spare bus at the `betterbanana_stream` null
sink and route the strips you want shared to it:

    bb-ctl route out A3 betterbanana_stream     # A3 becomes the stream bus
    bb-ctl strip 1 bus A3 1           # game / app audio -> stream
    bb-ctl strip 3 bus A3 1           # music -> stream

**Never route AUX to the stream bus.** That is the echo, straight back.

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

## Control from the shell

    ./build/bb-ctl status
    ./build/bb-ctl meters 20
    ./build/bb-ctl strip 3 gain -6
    ./build/bb-ctl route out A1 alsa_output.usb-...-00.HiFi__Line__sink
    ./build/bb-ctl rec bus B1 && ./build/bb-ctl rec file take.wav && ./build/bb-ctl rec start
    ./build/bb-ctl vban out 1 host 192.168.1.20 && ./build/bb-ctl vban out 1 on
    ./build/bb-ctl vban apply

## Tests

    ./build/test_dsp          # 24 DSP assertions, no audio server needed
    ./tests/integration.sh    # drives real audio through a running engine

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
compressor / 3-band EQ / audibility / Intellipan / mono / solo / mute / fader,
per-bus EQ / mono / mute / fader, metering, hardware assignment, the tape deck,
and VBAN send/receive.

Not implemented: the surround bus modes, which need buses wider than stereo.
