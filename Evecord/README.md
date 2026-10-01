# Evecord

Discord with [Vencord](https://vencord.dev) built in, as its own desktop app, made
for a Linux desktop running Wayland and PipeWire. Modelled on
[Vesktop](https://github.com/Vencord/Vesktop) and written from scratch.

It runs Discord's web app in Electron, the way Vesktop does. It is not the official
client with a patch on top. That means:

- **Screen share with audio.** Pick the apps (or a mixer bus) whose sound goes out
  with the stream. Wayland screen capture goes through the desktop portal.
- **Your own Vencord.** If `~/Projects/Vencord/dist` holds a build, Evecord loads it,
  userplugins included. Without one, it downloads the official release.
- **Rebuild, then reload.** Run `pnpm build` in the Vencord checkout and Evecord shows
  a banner offering a reload (or a restart, if Vencord's main process changed).
- **No native voice engine.** Voice and video go through Chromium's WebRTC, so
  `discord_voice.node` and its NVENC encoder (and that encoder's crash) are not
  involved at all.
- **Leaves your media keys alone.** Discord does not register as an MPRIS player.

## Install

```sh
cd "Linux-Apps/Evecord" && ./install.sh
```

Needs `node` 22+ and `pnpm`. Installs to `~/.local/lib/evecord`, with
`~/.local/bin/evecord` and an app-menu entry. Re-run it to update;
`./install.sh --uninstall` removes it (settings stay in `~/.config/evecord`).

The first start asks a few questions. It also offers to copy your Vencord settings
and themes from the Discord app's `~/.config/Vencord`, so plugins come over
configured. After that, Evecord's options live in Discord's settings under
**Vencord → Evecord**.

## Screen share audio

When you go live, the picker lists:

- **Apps**: one entry per app, e.g. Spotify or Firefox.
- **Buses**: virtual sources, e.g. *BetterBanana_Stream_Bus*. Pick a BetterBanana
  bus to send exactly what you mixed onto it.
- **Entire System**: everything playing to your default output, minus Evecord itself.

Behind the picker, [venmic](https://github.com/Vencord/venmic) creates a PipeWire
node called `vencord-screen-share`, links the chosen sources into it, and Evecord
attaches that node to the stream as its audio track. Discord's device lists never
show the node.

venmic's "only speakers" filters apply only to Entire System. A source you pick by
name is always linked, even when it plays into a virtual cable rather than a device.
(Vesktop applies those filters to picked sources too, so apps routed through a mixer
stay silent there.)

## Keys

| | |
|---|---|
| Ctrl+R / F5 | reload Discord |
| Ctrl+Shift+R | restart Evecord |
| Ctrl+Shift+I / F12 | DevTools |
| Ctrl+= / Ctrl+- / Ctrl+0 | zoom |
| Ctrl+Q | quit (closing the window only hides it to the tray) |

## Files

| | |
|---|---|
| `~/.config/evecord/evecord.json` | Evecord's settings |
| `~/.config/evecord/settings/`, `themes/` | Vencord's settings, QuickCSS and themes |
| `~/.config/evecord/session/` | the browser profile: login, caches |
| `~/.config/evecord/vencord-dist/` | downloaded Vencord, used when there is no local build |

`EVECORD_DATA_DIR=/some/dir evecord` runs with a separate profile.

## Development

```sh
pnpm install && node node_modules/electron/install.js
pnpm start          # dev build + run
pnpm watch          # rebuild on change (Ctrl+R in the app picks up renderer changes)
pnpm test           # type check + unit tests
```

- `src/main`: the Electron main process. Window, tray, IPC, Vencord loading,
  screen share, venmic, arRPC.
- `src/preload`: exposes `EvecordNative` to the page, then injects Vencord and
  Evecord's renderer.
- `src/renderer`: runs inside Discord, on Vencord's APIs. The webpack patches,
  the screen share picker, the settings page.

Evecord exposes its bridge as `VesktopNative` too, because Vencord's desktop build
calls that name.

If a Discord update breaks something, Vencord logs `Patch by Evecord had no effect`
in DevTools. The patches are in `src/renderer/patches/`.

## License

GPL-3.0-or-later, like Vesktop and Vencord, which it is modelled on and runs.
