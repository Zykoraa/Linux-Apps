# Eve's Garden

A music player and downloader for Linux. It plays the music you already have
-- MP3, FLAC, M4A, Opus, Ogg and more, from wherever you keep it -- and
fetches what you do not, looking up track, album and artwork details and
tagging the result properly.

Paste a Bandcamp, Internet Archive or SoundCloud link and it is fetched from
there -- which is how you get lossless, since YouTube has never served any:
its best is Opus at about 170kbps, and wrapping that in a FLAC would be four
times the size for not one bit more music.

Downloads keep the quality they were served at instead of being re-encoded
into something smaller and worse, and every track says what it actually is
-- "Opus 141 kbps", "FLAC 16/44.1 kHz" -- in the queue and while it plays.
Playback is gapless, with crossfade, a 10-band EQ, synced lyrics, nine
visualisers and Discord Rich Presence -- and volume levelling, so a
loudness-war master and a quiet remaster sit at the same level instead of
sending you to the volume knob between tracks.

No account or API key is needed to search and download. Connecting Spotify is
optional and adds your own playlists and liked songs -- and then watches them.
Spotify playlists lose tracks as licensing lapses, and nothing tells you: the
playlist is just shorter than you remember and the name of the song is gone.
This wrote down what was in it and kept the audio, so it can say what went,
and that you still have it.

![icon](assets/icon.png)

## Install

```sh
git clone https://github.com/Zykoraa/Linux-Apps.git
cd "Linux-Apps/EvesGarden" && ./install.sh
```

No root. It installs to `~/.local`, so **Eve's Garden** appears in your app
menu (wofi, rofi, fuzzel, GNOME, KDE) and `evesgarden` runs it from a
terminal. Re-run `./install.sh` to update; `./install.sh --uninstall` removes
it and leaves your music and settings alone.

The installer checks for what it needs first and names the package for your
distribution if anything is missing. In advance, if you would rather:

| | |
| --- | --- |
| Arch / CachyOS | `sudo pacman -S python tk ffmpeg portaudio base-devel` |
| Debian / Ubuntu | `sudo apt install python3 python3-tk python3-venv python3-dev ffmpeg portaudio19-dev build-essential` |
| Fedora | `sudo dnf install python3 python3-tkinter python3-devel ffmpeg portaudio-devel gcc` |
| openSUSE | `sudo zypper install python3 python3-tk python3-devel ffmpeg portaudio-devel gcc` |

`portaudio` and a compiler are needed because PyAudio has no Linux wheel and
is built against your system's PortAudio during install.

Nothing to configure. Searching, downloading and playback all work on first
launch. Music downloads to `~/Music/Eve's Garden`.
[Connecting Spotify](#connecting-spotify-optional) is optional -- it adds
your own playlists and liked songs.

There is also a self-contained `.tar.gz` on the
[releases page](https://github.com/Zykoraa/Linux-Apps/releases), for x86_64
and glibc 2.35 or newer. `install.sh` is the better option where it works:
it builds against the system you actually have, and does not carry a second
copy of every library that then never gets an update.

## Desktop integration

The player publishes itself over **MPRIS**, which is how a Linux desktop
expects to be told what is playing. So, with no configuration:

* **media keys work** — play/pause, next and previous are routed to it by
  your compositor. On GNOME and KDE that is immediate. On a bare
  Hyprland or sway setup the keys are usually bound through `playerctl`, so
  `sudo pacman -S playerctl` (or your distribution's equivalent) and bind
  `playerctl play-pause` if you have not already.
* **`playerctl` and anything built on it** control it —
  `playerctl -p evesgarden metadata`, `status`, `position 30`, and so on.
* **panels show the track** — waybar's `mpris` module, GNOME's media
  controls, KDE's applet and the lock screen get the title, artist, album
  art and working transport buttons.

Artwork reaches the panel even for a track nothing online has heard of: the
embedded cover is written out to `~/.cache/evesgarden/covers` and handed over
as a local file, so it works with no network at all.

Double-clicking an audio file in a file manager offers Eve's Garden in
*Open With*, and opening one plays it.

## What it does

**Library** — a SQLite index over the tags of your files, so you can browse
by song, album or artist, sort by year or play count, and search across
title, artist and album rather than just filenames.

**Downloading** — paste a Spotify track, album or playlist link, or just
search. Candidate YouTube sources are scored against the Spotify track's
duration and penalised for `live`, `karaoke`, `remix`, `full album` and the
like, so you get the right version rather than the first hit. Downloads run
three at a time with per-track state, cancel, and retry-failed.

**Playback** — a serial cascade of RBJ peaking biquads for the EQ (unity at
0 dB, so "flat" really is flat), soft-clipping to round off peaks, volume,
seek, shuffle, repeat, and resume where you left off. Audio goes out through
PortAudio, so PipeWire, PulseAudio and bare ALSA all work.

**Presentation** — 18 themes, 32 visualiser modes with 13 colour palettes,
synced lyrics, and album-art-derived accent colours with a contrast check.

## Where it keeps things

Per the XDG Base Directory spec, so the parts that matter are the parts that
get backed up:

| | |
| --- | --- |
| `~/Music/Eve's Garden` | downloads (or wherever `xdg-user-dir MUSIC` points) |
| `~/.config/evesgarden` | settings and credentials |
| `~/.local/share/evesgarden` | the library index |
| `~/.cache/evesgarden` | cover art and the Spotify token cache |
| `~/.local/state/evesgarden` | logs |

Deleting music from the Duplicates view sends it to your desktop's trash --
the real one, which Nautilus, Dolphin, Thunar and Nemo all read, so it
restores from there like anything else you deleted.

## Connecting Spotify (optional)

Track details come from Apple's iTunes catalogue, which needs no account, so
none of this is required to download music.

Connecting Spotify adds one thing nothing else can: your own library -- your
playlists and your liked songs. It takes free credentials of your own, which
the app asks for and writes itself:

1. Create an app at <https://developer.spotify.com/dashboard> — any name, no
   card required
2. In that app's **Settings**, add this exact Redirect URI:
   `http://127.0.0.1:8888/callback`
   (already have a redirect you would rather reuse? Put it in
   `SPOTIPY_REDIRECT_URI` instead — any loopback address and path works)
3. Copy the Client ID and Client Secret into the app's "Connect Spotify"
   screen

### Playlists need a sign-in

Spotify no longer lets an app read playlists — even public ones — without a
signed-in user. Press **Sign in to Spotify** in the downloader, approve it in
the browser once, and playlist links start working. The token is cached, so
it only asks the first time.

Three read-only scopes are requested: `playlist-read-private`,
`playlist-read-collaborative` and `user-library-read` (the last one is what
makes **Liked Songs** downloadable — paste
`https://open.spotify.com/collection/tracks`).

**Spotify's own playlists cannot be downloaded by any app.** Discover Weekly,
Daily Mix, Release Radar, Today's Top Hits and the rest of Spotify's
editorial and algorithmic playlists were closed to the API in 2024 and return
404 even to the user they were made for. Copy the tracks into a playlist of
your own and use that instead.

Credentials are saved to `~/.config/evesgarden/env`. See `.env.example` to
write the file by hand instead. An `EvesGarden.env` next to `gui.py` is used
first if one exists, which keeps a portable checkout self-contained.

Discord Rich Presence needs no setup. It talks to the socket Discord opens in
`$XDG_RUNTIME_DIR`, so it works with the native client and the Flatpak.

## Running from source

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python gui.py
```

`ffmpeg` and `ffprobe` are found on your PATH. A `bin/` directory beside the
app is used first if it has them, which is only there for building a fully
offline archive.

## Tests

```sh
.venv/bin/python -m unittest discover -s tests -v
```

`test_core.py` and `test_gapless.py` are headless. `test_smoke.py` opens the
real window and walks every surface in it, so it needs a display -- it skips
itself without one. That one is worth running: Tk swallows exceptions raised
inside callbacks, so a broken panel leaves the app running and looking almost
right, and this is what catches that.

## Building the archive

```sh
.venv/bin/pip install pyinstaller
.venv/bin/python -m PyInstaller gui.spec --noconfirm
```

Output lands in `dist/evesgarden/`. Releases are built automatically: pushing
a tag like `evesgarden-v1.0.0` runs
[the workflow](../.github/workflows/evesgarden-release.yml), which runs the
tests, builds the archive and attaches it to a new GitHub release.

To change the app icon, edit `DESIGN` in `make_icon.py` (`Monstera`, `Leaf`,
`Bloom` or `Sprout`), run it, and re-install — it regenerates the hicolor
PNGs, the `.ico` and the base64 copy embedded in `app_icon.py`.

## Layout

| File | Purpose |
| --- | --- |
| `gui.py` | The app window, playback controls and overlays |
| `library_index.py` | SQLite index over the tags |
| `library_view.py` | Songs / Albums / Artists browser |
| `downloader.py` | Spotify metadata, YouTube sourcing, tagging |
| `download_manager.py` | Download queue with per-track state |
| `player_engine.py` | Decoding, EQ, playback |
| `visualizers.py` | 32 visualiser modes and 13 palettes |
| `mpris.py` | The player on D-Bus: media keys, panels, `playerctl` |
| `linux_window.py` | Frameless window that the window manager still owns |
| `recycle.py` | Trashing files, per the freedesktop spec |
| `xdg_paths.py` | Where everything is kept |
| `ffmpeg_tools.py` | Finding ffmpeg |
| `discord_presence.py` | Rich Presence |
| `credentials.py` | Where credentials are read from and written to |
| `settings.py` | Persisted UI state |
| `make_icon.py` | Generates the app icon |

## Notes

This started as a Windows app. What the port changed, and why, is written
into the modules it changed -- `mpris.py` on why claiming the media keys is
the wrong shape here, `linux_window.py` on why `overrideredirect` is a
different and much stronger thing on X11, `recycle.py` on why the Windows
fallback would have deleted your music.

Credentials are never compiled into the binary. A client secret inside an
executable can be read straight back out of the archive, so each person
supplies their own.

For personal use. You are responsible for respecting the terms of the
services it talks to.
