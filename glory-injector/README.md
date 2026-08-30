# Glory Injector

A themed Linux **shared-object (`.so`) injector** with a Tk GUI. Select a running
process (or a window, via Hyprland's IPC), pick a `.so`, and it's loaded into the
target using the documented approach: `gdb` attaches and calls `dlopen()` inside
the process, verified against `/proc/<pid>/maps`.

Ported from a Windows build; developed for **CachyOS + Hyprland**, but runs on any
Arch-based system with X11/XWayland.

> Library injection is a standard, dual-use technique (debugging, modding,
> instrumentation, research). Use it on processes **you own on your own machine**.

---

## Features

- Process list from `/proc`, and an **Applications** tab (windows + titles) via `hyprctl`.
- Injection via `gdb` → `dlopen()` / `__libc_dlopen_mode()`, with a `/proc/<pid>/maps` success check.
- Runs unprivileged when `ptrace_scope=0`; otherwise **self-elevates just `gdb`** through `pkexec` (no root GUI).
- Live search/filter, dark themed UI, animated flourishes.

## Requirements

| Purpose | Package |
|---|---|
| GUI runtime | `python`, `tk` |
| Injection | `gdb` |
| Applications tab | `hyprland` (provides `hyprctl`) |
| Bibi button image (optional) | `python-pillow` |
| Elevate `gdb` without root login | `polkit` + an agent (e.g. `hyprpolkitagent`) |

---

## Install

### Prebuilt package (easiest — no build)

Every version tag publishes a ready-built package on the
[Releases](https://github.com/Zykoraa/Linux-Apps/releases) page. Download the `.pkg.tar.zst` and:

```bash
sudo pacman -U ./glory-injector-*-any.pkg.tar.zst
```

…or install straight from a release URL:

```bash
sudo pacman -U https://github.com/Zykoraa/Linux-Apps/releases/download/v1.0.0/glory-injector-1.0.0-1-any.pkg.tar.zst
```

### A. Build the pacman package yourself

```bash
git clone https://github.com/Zykoraa/Linux-Apps.git
cd Linux-Apps/glory-injector
makepkg -si
```

`makepkg -s` pulls the runtime deps; `-i` installs the package. You need
`base-devel` first: `sudo pacman -S --needed base-devel`.

It's then tracked by pacman:

```bash
pacman -Qi glory-injector          # info
sudo pacman -Rns glory-injector    # clean removal
```

### B. User install, no root (`~/.local`)

```bash
./install.sh              # installs launcher + icon + desktop entry
./install.sh --uninstall  # remove
```

### C. Run directly

```bash
python glory_injector_linux.py
```

---

## Permissions (attaching to processes)

Injection needs permission to `ptrace` the target — the Linux equivalent of
"Run as Administrator". Pick one:

**Temporary (until reboot):**
```bash
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
```

**Permanent (survives reboot):**
```bash
echo 'kernel.yama.ptrace_scope = 0' | sudo tee /etc/sysctl.d/10-ptrace.conf
sudo sysctl --system
```

**Or don't lower it at all** — with a polkit agent running, the app elevates only
`gdb` via a `pkexec` prompt when you inject.

> `ptrace_scope=0` lets any process running as your user read the memory of your
> other processes (the pre-Yama default). Reasonable on a personal dev box; know
> the trade-off.

---

## Usage

1. Launch **Glory Injector** (app menu, or `glory-injector`).
2. **⟳ Refresh**, then select a target in *Applications* or *All Processes*.
3. **Browse…** to a `.so`, then **Execute**.
4. The log area reports success or the exact reason it failed.

### Try it with the bundled payload

```bash
gcc -shared -fPIC -o /tmp/payload_example.so payload_example.c
```

Inject `/tmp/payload_example.so` into a throwaway app, then watch it prove itself:

```bash
tail -f /tmp/glory_injected.log
```

The `.so` must be a **64-bit Linux** object and the target **dynamically linked**
(most GUI apps are). Statically linked binaries can't be injected this way.

---

## Troubleshooting

| Log message | Fix |
|---|---|
| `gdb not found` | `sudo pacman -S gdb` |
| `permission denied` / ptrace hint | Lower `ptrace_scope` (above) or run a polkit agent |
| `dlopen returned NULL` | Wrong arch or missing deps in your `.so` |
| `statically linked` | Pick a different, dynamically-linked target |
| Applications tab empty | Not on Hyprland, or `hyprctl` missing |

---

## Notes

- The app runs as your normal user; only the `gdb` injection step escalates.
- Hyprland window rule, if you want it floating:
  `windowrule = float, class:(GloryInjector)`
- `netanyahu.jpg` is **not** shipped (drop your own next to the script to enable
  the image button; it falls back to a ✡ glyph otherwise).

## License

MIT — see the repository [LICENSE](../LICENSE).
