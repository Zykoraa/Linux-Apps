#!/usr/bin/env python3
"""
Glory Injector (Linux port) - מזרק התהילה
Personal shared-object (.so) injector with themed GUI.

Ported from the Windows edition. On Linux there is no CreateRemoteThread /
LoadLibraryA, so injection is done the documented way: attach to the target
with gdb and have it call dlopen() (falling back to glibc's
__libc_dlopen_mode) inside the process, then verify the mapping via
/proc/<pid>/maps.

Target platform: CachyOS + Hyprland (Wayland).

Dependencies
    - python + tkinter        (pacman -S tk)
    - gdb                     (pacman -S gdb)       - required for injection
    - hyprctl                 (comes with Hyprland) - Applications tab
    - python-pillow           (pacman -S python-pillow, optional - Bibi image)

Permissions
    Attaching to another running process needs privilege, the same way the
    Windows build needed Administrator:
      * run the app as root (sudo -E ./glory_injector_linux.py), OR
      * have a polkit agent running so the app can elevate gdb via pkexec, OR
      * lower Yama:  echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
"""

import os
import sys
import re
import json
import math
import time
import shutil
import random
import datetime
import threading
import subprocess
import webbrowser

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
from tkinter import font as tkfont

try:
    from PIL import Image, ImageTk
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


# ─── Status glyphs ──────────────────────────────────────────────────
OK   = "✓"   # ✓
NO   = "✗"   # ✗
WARN = "⚠"   # ⚠

# ─── dlopen flags (from <dlfcn.h>) ──────────────────────────────────
RTLD_LAZY   = 0x1
RTLD_NOW    = 0x2
RTLD_GLOBAL = 0x100
RTLD_DLOPEN = 0x80000000   # __RTLD_DLOPEN, ORed in for __libc_dlopen_mode


# ─── Fonts (resolved at runtime against installed families) ─────────
FONT_UI    = "DejaVu Sans"
FONT_UI_SB = "DejaVu Sans"       # "semibold" -> rendered with weight "bold"
FONT_MONO  = "DejaVu Sans Mono"


def _resolve_fonts(root):
    """Pick the nicest installed family for each role; degrade gracefully."""
    global FONT_UI, FONT_UI_SB, FONT_MONO
    try:
        fams = {f.lower() for f in tkfont.families(root)}
    except Exception:
        return

    def pick(cands, fallback):
        for c in cands:
            if c.lower() in fams:
                return c
        return fallback

    FONT_UI = pick(
        ["Inter", "Cantarell", "Noto Sans", "Ubuntu",
         "DejaVu Sans", "Liberation Sans", "Roboto"], FONT_UI)
    FONT_UI_SB = pick(
        ["Inter SemiBold", "Inter Semi Bold", "Cantarell", "Noto Sans",
         "Ubuntu", "DejaVu Sans", "Liberation Sans"], FONT_UI)
    FONT_MONO = pick(
        ["JetBrainsMono Nerd Font", "JetBrains Mono", "Fira Code",
         "Fira Mono", "Hack", "DejaVu Sans Mono", "Liberation Mono",
         "Noto Sans Mono"], FONT_MONO)


# ─── Process enumeration (/proc) ────────────────────────────────────
def _proc_name(pid):
    """Best human-readable name for a pid, or None if it vanished."""
    # Real executable (nicest, when we're allowed to read the symlink)
    try:
        exe = os.readlink(f"/proc/{pid}/exe")
        base = os.path.basename(exe)
        if base:
            return base.split(" (deleted)")[0] or base
    except OSError:
        pass
    # comm (always world-readable, but truncated to 15 chars)
    try:
        with open(f"/proc/{pid}/comm", "r", encoding="utf-8", errors="replace") as f:
            name = f.read().strip()
            if name:
                return name
    except OSError:
        pass
    # cmdline[0] basename
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            first = f.read().split(b"\x00")[0]
            if first:
                return os.path.basename(first.decode("utf-8", "replace"))
    except OSError:
        pass
    return None


def enumerate_processes():
    procs = []
    try:
        entries = os.listdir("/proc")
    except OSError:
        return procs
    for entry in entries:
        if not entry.isdigit():
            continue
        pid = int(entry)
        name = _proc_name(pid)
        if name:
            procs.append((pid, name))
    procs.sort(key=lambda p: p[1].lower())
    return procs


# ─── Hyprland IPC helpers ───────────────────────────────────────────
def _hyprctl_json(cmd):
    """Return parsed JSON from `hyprctl <cmd> -j`, or None."""
    if not shutil.which("hyprctl"):
        return None
    try:
        out = subprocess.run(["hyprctl", cmd, "-j"],
                             capture_output=True, text=True, timeout=5)
        if out.returncode != 0:
            return None
        return json.loads(out.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return None


def _hyprctl(*args):
    """Fire-and-forget hyprctl dispatch; best effort."""
    if not shutil.which("hyprctl"):
        return
    try:
        subprocess.run(["hyprctl", *args],
                       capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.SubprocessError):
        pass


def enumerate_applications():
    """Windows with titles, via Hyprland's client list."""
    apps = []
    data = _hyprctl_json("clients")
    if not data:
        return apps
    seen = set()
    for cl in data:
        pid = cl.get("pid", -1)
        title = (cl.get("title") or "").strip()
        cls = (cl.get("class") or "").strip()
        if pid <= 0 or not title:
            continue
        if not cl.get("mapped", True):
            continue
        if pid in seen:
            continue
        seen.add(pid)
        name = _proc_name(pid) or cls or "<unknown>"
        apps.append((pid, name, title))
    apps.sort(key=lambda a: a[2].lower())
    return apps


# ─── Injection core (gdb → dlopen) ──────────────────────────────────
def _ptrace_scope():
    try:
        with open("/proc/sys/kernel/yama/ptrace_scope") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def _module_in_maps(pid, so_path):
    base = os.path.basename(so_path)
    try:
        with open(f"/proc/{pid}/maps") as f:
            for line in f:
                if base in line:
                    return True
    except OSError:
        pass
    return False


def _is_root():
    try:
        return os.geteuid() == 0
    except AttributeError:
        return False


def _elevation_prefixes():
    """Command prefixes to try, in order. Empty prefix == current user."""
    if _is_root():
        return [[]]
    prefixes = [[]]                      # try unprivileged first (ptrace_scope=0)
    if shutil.which("pkexec"):
        prefixes.append(["pkexec"])      # graphical polkit prompt, runs gdb as root
    elif shutil.which("sudo"):
        prefixes.append(["sudo", "-n"])  # only helps if passwordless (no GUI prompt)
    return prefixes


def _gdb_call(prefix, gdb_path, pid, expr):
    """
    Run one gdb attach + `print <expr>` and interpret the result.
    Returns (ran, nonnull, detail):
        ran     - the function actually executed in the target
        nonnull - it returned a non-NULL pointer (success)
        detail  - short human string for logging
    """
    argv = list(prefix) + [
        gdb_path, "-q", "-nx", "-batch",
        "-ex", "set pagination off",
        "-ex", "set confirm off",
        "-ex", f"attach {pid}",
        "-ex", f"print {expr}",
        "-ex", "detach",
        "-ex", "quit",
    ]
    timeout = 200 if prefix else 60      # allow time for a polkit prompt
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return (False, False, "gdb timed out")
    except OSError as e:
        return (False, False, f"failed to launch gdb: {e}")

    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    low = out.lower()

    if "no symbol" in low and "in current context" in low:
        return (False, False, "symbol unavailable")
    if "not permitted" in low or "permission denied" in low:
        return (False, False, "operation not permitted")
    if "ptrace:" in low and "could not attach" in low:
        return (False, False, "operation not permitted")

    m = re.search(r"=\s*\(void\s*\*\)\s*(0x[0-9a-fA-F]+)", out)
    if m:
        addr = int(m.group(1), 16)
        return (True, addr != 0, f"returned {hex(addr)}")

    tail = out.strip().splitlines()[-1] if out.strip() else "no output"
    return (False, False, f"unrecognized gdb output: {tail[:160]}")


def _gdb_inject(prefix, gdb_path, pid, so_path):
    esc = so_path.replace("\\", "\\\\").replace('"', '\\"')

    # Attempt 1: dlopen (exported from libc on modern glibc / Arch / CachyOS)
    expr1 = f'(void *) dlopen("{esc}", {hex(RTLD_NOW | RTLD_GLOBAL)})'
    ran, nonnull, detail = _gdb_call(prefix, gdb_path, pid, expr1)
    if ran and nonnull:
        return (True, True, f"dlopen {detail}")
    if "permitted" in detail:
        return (False, False, detail)

    # Attempt 2: glibc's internal loader entry point
    expr2 = f'(void *) __libc_dlopen_mode("{esc}", {hex(RTLD_NOW | RTLD_DLOPEN)})'
    ran2, nonnull2, detail2 = _gdb_call(prefix, gdb_path, pid, expr2)
    if ran2 and nonnull2:
        return (True, True, f"__libc_dlopen_mode {detail2}")
    if "permitted" in detail2:
        return (False, False, detail2)

    if detail == "symbol unavailable" and detail2 == "symbol unavailable":
        return (False, False, "symbol unavailable")
    if ran:
        return (True, False, f"dlopen {detail}")
    if ran2:
        return (True, False, f"__libc_dlopen_mode {detail2}")
    return (False, False, detail or detail2)


def inject_so(pid, so_path):
    so_path = os.path.abspath(os.path.expanduser(so_path))
    if not os.path.isfile(so_path):
        return f"{NO} File not found: {so_path}"
    gdb_path = shutil.which("gdb")
    if not gdb_path:
        return f"{NO} gdb not found — install it:  sudo pacman -S gdb"
    if not os.path.isdir(f"/proc/{pid}"):
        return f"{NO} No such process: PID {pid}"
    if _module_in_maps(pid, so_path):
        return f"{OK} Already loaded in PID {pid}: {os.path.basename(so_path)}"

    permission_problem = False
    last = "no attempt made"
    for prefix in _elevation_prefixes():
        ran, nonnull, detail = _gdb_inject(prefix, gdb_path, pid, so_path)
        last = detail
        if _module_in_maps(pid, so_path) or nonnull:
            tag = "  (elevated)" if prefix else ""
            return (f"{OK} Injected successfully — "
                    f"{os.path.basename(so_path)} loaded into PID {pid}{tag}")
        if "permitted" in detail:
            permission_problem = True
            continue
        if detail == "symbol unavailable":
            return (f"{NO} dlopen/__libc_dlopen_mode not found in PID {pid} — "
                    f"the target may be statically linked or not use glibc.")
        if "0x0" in detail or "NULL" in detail.upper():
            return (f"{NO} dlopen returned NULL — the target could not load "
                    f"{os.path.basename(so_path)}. Confirm it is a 64-bit Linux "
                    f".so and its dependencies are present.")
        # unknown failure: keep trying any elevated prefixes

    if permission_problem:
        return f"{NO} Cannot attach to PID {pid} (permission denied).\n{_ptrace_hint()}"
    return f"{NO} Injection failed: {last}"


def _ptrace_hint():
    scope = _ptrace_scope()
    lines = ["   Need permission to attach to the target process."]
    if scope not in (0, None):
        lines.append(f"   ptrace_scope is {scope}. Allow attaching with:")
        lines.append("     echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope")
    lines.append("   Or run a polkit agent (so gdb can elevate via pkexec),")
    lines.append("   or launch the whole app with sudo.")
    return "\n".join(lines)


# ─── Terminal / window-shake helpers (traitor sequence) ─────────────
SCAN_BODY = (
    "printf '\\033]0;SCANNING\\007'; "
    "printf '\\033[2J\\033[1;32m'; "
    "echo; echo '   >> SECURITY SCAN IN PROGRESS <<'; echo; "
    "find / -xdev 2>/dev/null | head -n 200000; "
    "printf '\\033[0m'; echo; "
    "read -n 1 -r -s -p 'Press any key to close...'"
)

TRAITOR_BODY = (
    "printf '\\033]0;TRAITOR\\007'; "
    "printf '\\033[2J\\033[1;31m'; "
    "echo; echo; "
    "echo '   TRAITOR DETECTED AND LOGGED -- TEL AVIV NOTIFIED'; "
    "echo; echo; "
    "printf '\\033[0m'; "
    "read -n 1 -r -s -p 'Press any key to close...'"
)


def _detect_terminal():
    for t in ("kitty", "alacritty", "foot", "wezterm", "konsole",
              "gnome-terminal", "xfce4-terminal", "xterm"):
        if shutil.which(t):
            return t
    return None


def _spawn_terminal(body):
    """Open a terminal emulator running `bash -c body`. Best effort."""
    term = _detect_terminal()
    if not term:
        return None
    run = ["bash", "-c", body]
    layouts = {
        "kitty":          ["kitty"] + run,
        "alacritty":      ["alacritty", "-e"] + run,
        "foot":           ["foot"] + run,
        "wezterm":        ["wezterm", "start", "--"] + run,
        "konsole":        ["konsole", "-e"] + run,
        "gnome-terminal": ["gnome-terminal", "--"] + run,
        "xfce4-terminal": ["xfce4-terminal", "-x"] + run,
        "xterm":          ["xterm", "-e"] + run,
    }
    argv = layouts.get(term, ["xterm", "-e"] + run)
    try:
        return subprocess.Popen(argv)
    except OSError:
        return None


def _focused_monitor_size():
    data = _hyprctl_json("monitors")
    if data:
        chosen = next((m for m in data if m.get("focused")), data[0])
        try:
            return int(chosen.get("width", 1920)), int(chosen.get("height", 1080))
        except (TypeError, ValueError):
            pass
    return 1920, 1080


def _shake_window(title_match, seconds):
    """Jitter a Hyprland window around by title. Best effort, fully guarded."""
    if not shutil.which("hyprctl"):
        return
    mw, mh = _focused_monitor_size()
    addr = None
    end = time.time() + seconds
    while time.time() < end:
        if addr is None:
            data = _hyprctl_json("clients")
            if data:
                for c in data:
                    if title_match.lower() in (c.get("title", "") or "").lower():
                        addr = c.get("address")
                        if addr and not c.get("floating", False):
                            _hyprctl("dispatch", "togglefloating", f"address:{addr}")
                        break
        if addr:
            x = random.randint(0, max(1, mw - 500))
            y = random.randint(0, max(1, mh - 350))
            _hyprctl("dispatch", "movewindowpixel", f"exact {x} {y},address:{addr}")
        time.sleep(0.12)


# ─── Resolve base path (PyInstaller or script) ──────────────────────
def _base_path():
    if getattr(sys, 'frozen', False):
        return sys._MEIPASS
    return os.path.dirname(os.path.abspath(__file__))


# ─── Color Palette (Israel) ─────────────────────────────────────────
C = {
    "bg":          "#0a0e1a",
    "surface":     "#111827",
    "surface2":    "#1a2236",
    "border":      "#263050",
    "text":        "#e8ecf4",
    "text_dim":    "#5a6a8a",
    "blue":        "#0038B8",
    "blue_hi":     "#1a56d6",
    "blue_light":  "#4a8af5",
    "white":       "#FFFFFF",
    "gold":        "#C9A84C",
    "gold_hi":     "#DFC06A",
    "accent":      "#4a8af5",
    "accent_hi":   "#6aa0ff",
    "green":       "#5CB85C",
    "red":         "#E74C3C",
    "yellow":      "#F5C842",
}
FLAG_STRIPES = ["#0038B8", "#FFFFFF", "#0038B8"]


# ─── Text Mappings ──────────────────────────────────────────────────
TEXTS = {
    "he": {
        "title_a":      "תהילה",
        "title_b":      " מזרק",
        "window_title": "מזרק התהילה",
        "subtitle":     "זהה בוגדים  ·  הפעל קוד קדוש  ·  תהילה לישראל",
        "tab_apps":     "  יישומים  ",
        "tab_procs":    "  כל התהליכים  ",
        "col_pid":      "PID",
        "col_process":  "תהליך",
        "col_title":    "כותרת חלון",
        "col_pname":    "שם תהליך",
        "refresh":      "⟳  רענן",
        "dll_label":    "SO:",
        "browse":       "עיון …",
        "execute":      "✡  הפעל  ➜",
        "ok_title":     "✡  אושר",
        "ok_msg":       "נתניהו מאשר את הבקשה הזו.",
        "fail_title":   "⚠  בוגד זוהה",
        "fail_msg":     "בוגד זוהה\n\nכתובת ה-IP של הפגישה תועדה\nתל אביב קיבלה הודעה.\n\nאל תנסה לעזוב את המדינה.",
    },
    "en": {
        "title_a":      "Glory",
        "title_b":      " Injector",
        "window_title": "Glory Injector",
        "subtitle":     "Identify Traitors  ·  Execute Holy Code  ·  Glory to Israel",
        "tab_apps":     "  Applications  ",
        "tab_procs":    "  All Processes  ",
        "col_pid":      "PID",
        "col_process":  "Process",
        "col_title":    "Window Title",
        "col_pname":    "Process Name",
        "refresh":      "⟳  Refresh",
        "dll_label":    "SO:",
        "browse":       "Browse …",
        "execute":      "✡  Execute  ➜",
        "ok_title":     "✡  APPROVED",
        "ok_msg":       "Netanyahu approves this request.",
        "fail_title":   "⚠  TRAITOR DETECTED",
        "fail_msg":     "TRAITOR DETECTED\n\nSESSION IP ADDRESS HAS BEEN RECORDED\nTEL AVIV NOTIFIED.\n\nDO NOT ATTEMPT TO LEAVE THE COUNTRY.",
    },
}


# ─── GUI Application ────────────────────────────────────────────────
class InjectorApp:
    _cache: list = []
    _app_cache: list = []

    def __init__(self, root: tk.Tk):
        self.root = root
        _resolve_fonts(root)
        self.lang = "he"  # Start in Hebrew
        self.root.title(TEXTS["he"]["window_title"])
        self.root.geometry("760x720")
        self.root.minsize(640, 620)
        self.root.configure(bg=C["bg"])

        self.dll_path = tk.StringVar()
        self.selected_pid = None
        self._english_unlocked = False

        # Track all translatable widgets: list of (widget, text_key, method)
        self._translatable = []

        self._apply_styles()
        self._build_ui()
        self._refresh()
        self._startup_checks()
        self._animate_stripe()
        self._animate_star_3d()

    # ── Theme ────────────────────────────────────────────────────────
    def _apply_styles(self):
        s = ttk.Style(self.root)
        s.theme_use("clam")

        s.configure(".", background=C["bg"], foreground=C["text"],
                     fieldbackground=C["surface"], borderwidth=0)
        s.configure("TFrame",  background=C["bg"])
        s.configure("TLabel",  background=C["bg"], foreground=C["text"],
                     font=(FONT_UI, 10))
        s.configure("Title.TLabel", font=(FONT_UI_SB, 22, "bold"),
                     foreground=C["white"])
        s.configure("Blue.TLabel", font=(FONT_UI_SB, 22, "bold"),
                     foreground=C["accent"], background=C["bg"])
        s.configure("Dim.TLabel", font=(FONT_UI, 8),
                     foreground=C["gold"])
        s.configure("Accent.TLabel", background=C["bg"],
                     foreground=C["accent"], font=(FONT_UI, 10))
        s.configure("Gold.TLabel", background=C["bg"],
                     foreground=C["gold"], font=(FONT_UI_SB, 10, "bold"))

        # Notebook (tabs)
        s.configure("TNotebook", background=C["bg"], borderwidth=0)
        s.configure("TNotebook.Tab",
                     background=C["surface2"], foreground=C["text_dim"],
                     font=(FONT_UI_SB, 10, "bold"), padding=(16, 6),
                     borderwidth=0)
        s.map("TNotebook.Tab",
               background=[("selected", C["blue"]),
                           ("active", C["border"])],
               foreground=[("selected", C["white"]),
                           ("active", C["accent_hi"])])

        # Treeview
        s.configure("Treeview",
                     background=C["surface"], foreground=C["text"],
                     fieldbackground=C["surface"], rowheight=28,
                     font=(FONT_MONO, 10), borderwidth=0)
        s.configure("Treeview.Heading",
                     background=C["surface2"], foreground=C["accent"],
                     font=(FONT_UI_SB, 10, "bold"), borderwidth=0)
        s.map("Treeview",
               background=[("selected", C["accent"])],
               foreground=[("selected", C["white"])])

        # Inject button – gold
        s.configure("Accent.TButton",
                     background=C["gold"], foreground=C["bg"],
                     font=(FONT_UI_SB, 11, "bold"), padding=(20, 10))
        s.map("Accent.TButton",
               background=[("active", C["gold_hi"]),
                           ("disabled", C["border"])],
               foreground=[("disabled", C["text_dim"])])

        # Secondary buttons – blue tinted
        s.configure("Sec.TButton",
                     background=C["surface2"], foreground=C["accent"],
                     font=(FONT_UI_SB, 10, "bold"), padding=(12, 7))
        s.map("Sec.TButton",
               background=[("active", C["border"])],
               foreground=[("active", C["accent_hi"])])

        # Entry
        s.configure("TEntry", fieldbackground=C["surface"],
                     foreground=C["text"], insertcolor=C["accent"],
                     font=(FONT_UI, 10), padding=6)

        # Scrollbar
        s.configure("Vertical.TScrollbar",
                     background=C["surface2"], troughcolor=C["surface"],
                     borderwidth=0, relief="flat")

    # ── Layout ───────────────────────────────────────────────────────
    def _build_ui(self):
        M = 20  # margin
        t = TEXTS[self.lang]

        # ── Header ───────────────────────────────────────────────────
        hdr = ttk.Frame(self.root)
        hdr.pack(fill="x", padx=M, pady=(M, 4))
        self.lbl_title_a = ttk.Label(hdr, text=t["title_a"], style="Blue.TLabel")
        self.lbl_title_a.pack(side="left")
        self.lbl_title_b = ttk.Label(hdr, text=t["title_b"], style="Title.TLabel")
        self.lbl_title_b.pack(side="left")

        # 3D Star of David canvas in the top-right
        self._star_canv = tk.Canvas(hdr, width=54, height=54,
                                     bg=C["bg"], highlightthickness=0)
        self._star_canv.pack(side="right", padx=(0, 0))
        self._star_angle = 0.0
        self._star_glow = 0

        # Subtitle row
        sub = ttk.Frame(self.root)
        sub.pack(fill="x", padx=M, pady=(0, 2))
        self.lbl_subtitle = ttk.Label(sub, text=t["subtitle"], style="Dim.TLabel")
        self.lbl_subtitle.pack(side="left")

        # ── Israel flag stripe ────────────────────────────────────────
        self._stripe_frame = tk.Frame(self.root, height=6, bg=C["bg"])
        self._stripe_frame.pack(fill="x", padx=M, pady=(6, 12))
        self._stripe_canv = tk.Canvas(
            self._stripe_frame, height=6, bg=C["bg"],
            highlightthickness=0, bd=0)
        self._stripe_canv.pack(fill="x", expand=True)
        self._stripe_canv.bind("<Configure>", self._draw_stripe)
        self._stripe_offset = 0

        # ── Search + Refresh ─────────────────────────────────────────
        bar = ttk.Frame(self.root)
        bar.pack(fill="x", padx=M)

        ttk.Label(bar, text="\U0001f50d", style="Accent.TLabel").pack(side="left")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self._filter())
        ttk.Entry(bar, textvariable=self.search_var, width=28).pack(
            side="left", padx=(4, 0))
        self.btn_refresh = ttk.Button(bar, text=t["refresh"], style="Sec.TButton",
                   command=self._refresh)
        self.btn_refresh.pack(side="right")

        # ── Tabbed Process List ───────────────────────────────────────
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=M, pady=(8, 10))

        # --- Applications tab ---
        app_frame = ttk.Frame(self.notebook)
        self.notebook.add(app_frame, text=t["tab_apps"])

        self.app_tree = ttk.Treeview(
            app_frame, columns=("pid", "name", "title"),
            show="headings", selectmode="browse")
        self.app_tree.heading("pid",   text=t["col_pid"],      anchor="w")
        self.app_tree.heading("name",  text=t["col_process"],  anchor="w")
        self.app_tree.heading("title", text=t["col_title"],    anchor="w")
        self.app_tree.column("pid",   width=60,  stretch=False)
        self.app_tree.column("name",  width=160, stretch=False)
        self.app_tree.column("title", width=380, stretch=True)

        app_vsb = ttk.Scrollbar(app_frame, orient="vertical",
                                command=self.app_tree.yview)
        self.app_tree.configure(yscrollcommand=app_vsb.set)
        self.app_tree.pack(side="left", fill="both", expand=True)
        app_vsb.pack(side="right", fill="y")
        self.app_tree.bind("<<TreeviewSelect>>", self._on_tree_select)

        # --- All Processes tab ---
        proc_frame = ttk.Frame(self.notebook)
        self.notebook.add(proc_frame, text=t["tab_procs"])

        self.tree = ttk.Treeview(
            proc_frame, columns=("pid", "name"),
            show="headings", selectmode="browse")
        self.tree.heading("pid",  text=t["col_pid"],    anchor="w")
        self.tree.heading("name", text=t["col_pname"],  anchor="w")
        self.tree.column("pid",  width=80,  stretch=False)
        self.tree.column("name", width=460, stretch=True)

        vsb = ttk.Scrollbar(proc_frame, orient="vertical",
                            command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self._on_tree_select)

        self.notebook.bind("<<NotebookTabChanged>>", self._on_tab_changed)

        # ── SO Path ──────────────────────────────────────────────────
        dll_row = ttk.Frame(self.root)
        dll_row.pack(fill="x", padx=M, pady=(0, 8))

        self.lbl_dll = ttk.Label(dll_row, text=t["dll_label"], style="Gold.TLabel")
        self.lbl_dll.pack(side="left")
        ttk.Entry(dll_row, textvariable=self.dll_path).pack(
            side="left", fill="x", expand=True, padx=(6, 8))
        self.btn_browse = ttk.Button(dll_row, text=t["browse"], style="Sec.TButton",
                   command=self._browse)
        self.btn_browse.pack(side="right")

        # ── Bottom Row: Inject Button + Netanyahu Button ─────────────
        btn_row = ttk.Frame(self.root)
        btn_row.pack(fill="x", padx=M, pady=(0, 6))

        self.inject_btn = ttk.Button(btn_row, text=t["execute"],
                                      style="Accent.TButton",
                                      command=self._inject)
        self.inject_btn.pack(side="left")

        # Netanyahu face button
        self._bibi_img = None
        self._load_bibi_image()
        if self._bibi_img:
            self.bibi_btn = tk.Button(
                btn_row, image=self._bibi_img,
                bg=C["bg"], activebackground=C["surface2"],
                bd=0, highlightthickness=0, relief="flat",
                cursor="hand2", command=self._bibi_clicked)
        else:
            self.bibi_btn = tk.Button(
                btn_row, text="✡", font=(FONT_UI, 16),
                bg=C["bg"], fg=C["gold"], activebackground=C["surface2"],
                bd=0, highlightthickness=0, relief="flat",
                cursor="hand2", command=self._bibi_clicked)
        self.bibi_btn.pack(side="right")

        # ── Log Area ─────────────────────────────────────────────────
        self.log = tk.Text(self.root, height=4, bg=C["surface"],
                           fg=C["text"], font=(FONT_MONO, 9),
                           insertbackground=C["text"], relief="flat",
                           padx=10, pady=8, state="disabled", wrap="word",
                           highlightthickness=0)
        self.log.pack(fill="x", padx=M, pady=(0, M))
        self.log.tag_configure("ok",   foreground=C["green"])
        self.log.tag_configure("err",  foreground=C["red"])
        self.log.tag_configure("warn", foreground=C["yellow"])
        self.log.tag_configure("dim",  foreground=C["text_dim"])

    # ── Startup environment checks ───────────────────────────────────
    def _startup_checks(self):
        if not shutil.which("gdb"):
            self._write_log(
                "gdb not found — injection disabled. Install: sudo pacman -S gdb",
                "warn")
        if not shutil.which("hyprctl"):
            self._write_log(
                "hyprctl not found — Applications tab needs Hyprland.", "warn")
        scope = _ptrace_scope()
        if not _is_root() and scope not in (0, None):
            self._write_log(
                f"ptrace_scope={scope}: injection may need sudo/pkexec "
                f"or `echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope`.",
                "warn")

    # ── Load Netanyahu image ─────────────────────────────────────────
    def _load_bibi_image(self):
        if not HAS_PIL:
            return
        img_path = os.path.join(_base_path(), "netanyahu.jpg")
        if not os.path.isfile(img_path):
            return
        try:
            img = Image.open(img_path)
            img = img.resize((48, 48), Image.LANCZOS)
            self._bibi_img = ImageTk.PhotoImage(img)
        except Exception:
            self._bibi_img = None

    # ── 3D Spinning Star of David ────────────────────────────────────
    def _draw_star_3d(self):
        c = self._star_canv
        c.delete("all")
        cx, cy = 27, 27
        r = 20
        angle = self._star_angle

        # Pulsing glow
        glow = abs((self._star_glow % 60) - 30) / 30.0
        gb = int(0x00 + (0x4a - 0x00) * glow)
        gr = int(0x38 + (0x8a - 0x38) * glow)
        color = f"#{gb:02x}{gr:02x}f5"

        # Define two triangles in 3D (XY plane)
        tri_up_3d = []
        tri_dn_3d = []
        for i in range(3):
            a_up = math.radians(-90 + i * 120)
            a_dn = math.radians(90 + i * 120)
            tri_up_3d.append((r * math.cos(a_up), r * math.sin(a_up), 0))
            tri_dn_3d.append((r * math.cos(a_dn), r * math.sin(a_dn), 0))

        # Rotate around Y axis
        cos_a = math.cos(angle)
        sin_a = math.sin(angle)

        def project(x, y, z):
            # Rotate around Y
            rx = x * cos_a + z * sin_a
            rz = -x * sin_a + z * cos_a
            ry = y
            # Perspective projection
            d = 200
            scale = d / (d + rz)
            return cx + rx * scale, cy + ry * scale

        pts_up = [project(*p) for p in tri_up_3d]
        pts_dn = [project(*p) for p in tri_dn_3d]

        # Determine which triangle is in front based on Z
        z_up = sum(-p[0] * sin_a + p[2] * cos_a for p in tri_up_3d) / 3
        z_dn = sum(-p[0] * sin_a + p[2] * cos_a for p in tri_dn_3d) / 3

        # Draw back triangle first, then front
        if z_up <= z_dn:
            draw_order = [(pts_up, 0.4), (pts_dn, 1.0)]
        else:
            draw_order = [(pts_dn, 0.4), (pts_up, 1.0)]

        for pts, alpha in draw_order:
            coords = []
            for px, py in pts:
                coords.extend([px, py])
            # Simulate alpha by blending with bg
            r_c = int(int(color[1:3], 16) * alpha + 0x0a * (1 - alpha))
            g_c = int(int(color[3:5], 16) * alpha + 0x0e * (1 - alpha))
            b_c = int(int(color[5:7], 16) * alpha + 0x1a * (1 - alpha))
            blended = f"#{r_c:02x}{g_c:02x}{b_c:02x}"
            c.create_polygon(*coords, outline=blended, fill="", width=2)

    def _animate_star_3d(self):
        self._star_angle += 0.035  # ~3 second full rotation
        self._star_glow += 1
        self._draw_star_3d()
        self.root.after(33, self._animate_star_3d)  # ~30fps

    # ── Animated Israel stripe ───────────────────────────────────────
    def _draw_stripe(self, event=None):
        c = self._stripe_canv
        c.delete("all")
        w = c.winfo_width() or 700
        segment = w / 3
        offset = self._stripe_offset % (segment * 3)
        for i in range(-1, 5):
            color = FLAG_STRIPES[i % 3]
            x0 = i * segment - offset
            c.create_rectangle(x0, 0, x0 + segment + 1, 6,
                               fill=color, outline=color)

    def _animate_stripe(self):
        self._stripe_offset += 1
        self._draw_stripe()
        self.root.after(50, self._animate_stripe)

    # ── Netanyahu Password Dialog ────────────────────────────────────
    def _bibi_clicked(self):
        if self._english_unlocked:
            return
        pwd = simpledialog.askstring(
            "✡", "הכנס סיסמה:",
            show="*", parent=self.root)
        if pwd is None:
            return
        if pwd == "GloryToIsrael":
            self._english_unlocked = True
            self._animate_language_switch()
        else:
            self._traitor_sequence()

    # ── Hebrew → English Cascade Animation ───────────────────────────
    def _animate_language_switch(self):
        """Animate each UI text element letter-by-letter from Hebrew to English."""
        # List of (widget, key, set_method) for animation
        targets = [
            (self.lbl_title_a, "title_a", "label"),
            (self.lbl_title_b, "title_b", "label"),
            (self.lbl_subtitle, "subtitle", "label"),
            (self.btn_refresh, "refresh", "button"),
            (self.btn_browse, "browse", "button"),
            (self.inject_btn, "execute", "button"),
        ]

        # Animate targets sequentially with delays
        delay = 0
        for widget, key, wtype in targets:
            he_text = TEXTS["he"][key]
            en_text = TEXTS["en"][key]
            self._animate_text_swap(widget, he_text, en_text, wtype, delay)
            delay += 400  # stagger each element

        # Update window title after animation
        self.root.after(delay, lambda: self.root.title(TEXTS["en"]["window_title"]))

        # Update tab names
        self.root.after(delay + 100, lambda: self.notebook.tab(0, text=TEXTS["en"]["tab_apps"]))
        self.root.after(delay + 200, lambda: self.notebook.tab(1, text=TEXTS["en"]["tab_procs"]))

        # Update column headings
        self.root.after(delay + 300, self._switch_headings_to_english)

        # Update lang
        self.root.after(delay + 400, self._set_lang_en)

        # Log it
        self.root.after(delay + 500, lambda: self._write_log(
            "English mode unlocked.", "ok"))

    def _set_lang_en(self):
        self.lang = "en"

    def _switch_headings_to_english(self):
        t = TEXTS["en"]
        self.app_tree.heading("pid",   text=t["col_pid"])
        self.app_tree.heading("name",  text=t["col_process"])
        self.app_tree.heading("title", text=t["col_title"])
        self.tree.heading("pid",  text=t["col_pid"])
        self.tree.heading("name", text=t["col_pname"])

    def _animate_text_swap(self, widget, old_text, new_text, wtype, start_delay):
        """Letter-by-letter swap from old_text to new_text with flash effect."""
        max_len = max(len(old_text), len(new_text))
        padded_old = old_text.ljust(max_len)

        for i in range(max_len + 1):
            char_delay = start_delay + i * 60  # 60ms per character

            # Flash frame: show gold character at position i
            if i < max_len:
                flash_text = new_text[:i] + "█" + padded_old[i+1:]
                self.root.after(char_delay, lambda w=widget, t=flash_text.rstrip(), wt=wtype:
                    self._set_widget_text(w, t, wt, flash=True))

            # Settle frame: show the real character
            settled_text = new_text[:i+1] + (padded_old[i+1:] if i+1 < max_len else "")
            self.root.after(char_delay + 30, lambda w=widget, t=settled_text.rstrip(), wt=wtype:
                self._set_widget_text(w, t, wt, flash=False))

    def _set_widget_text(self, widget, text, wtype, flash=False):
        try:
            if wtype == "label":
                widget.configure(text=text)
                if flash:
                    orig_fg = widget.cget("foreground") if hasattr(widget, 'cget') else C["text"]
                    widget.configure(foreground=C["gold"])
                    self.root.after(30, lambda: widget.configure(
                        foreground=orig_fg))
            elif wtype == "button":
                widget.configure(text=text)
        except Exception:
            pass

    # ── Traitor Punishment Sequence ───────────────────────────────────
    def _traitor_sequence(self):
        """Wrong password: chaos mode (Linux/Hyprland edition)."""
        self._write_log(f"{WARN} UNAUTHORIZED ACCESS ATTEMPT", "err")

        def _run_punishment():
            # 1. Open a green "scanning" terminal
            _spawn_terminal(SCAN_BODY)
            time.sleep(0.6)

            # 2. Shake the scanning window around via Hyprland
            try:
                _shake_window("SCANNING", 5)
            except Exception:
                pass

            # 3. Open whatsmyip.com
            try:
                webbrowser.open("https://whatsmyip.com/")
            except Exception:
                pass

            time.sleep(0.4)

            # 4. Open a red terminal with the traitor message
            _spawn_terminal(TRAITOR_BODY)

        threading.Thread(target=_run_punishment, daemon=True).start()

    # ── Callbacks ────────────────────────────────────────────────────
    def _write_log(self, msg: str, tag: str = "dim"):
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        self.log.configure(state="normal")
        self.log.insert("end", f"[{ts}]  {msg}\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _refresh(self):
        InjectorApp._cache = enumerate_processes()
        InjectorApp._app_cache = enumerate_applications()
        self._populate_all(InjectorApp._cache)
        self._populate_apps(InjectorApp._app_cache)
        self._write_log(
            f"Found {len(InjectorApp._app_cache)} apps, "
            f"{len(InjectorApp._cache)} total processes", "dim")

    def _populate_all(self, procs):
        self.tree.delete(*self.tree.get_children())
        for pid, name in procs:
            self.tree.insert("", "end", iid=f"p{pid}", values=(pid, name))

    def _populate_apps(self, apps):
        self.app_tree.delete(*self.app_tree.get_children())
        for pid, name, title in apps:
            self.app_tree.insert(
                "", "end", iid=f"a{pid}", values=(pid, name, title))

    def _filter(self):
        q = self.search_var.get().lower()
        if not q:
            self._populate_all(InjectorApp._cache)
            self._populate_apps(InjectorApp._app_cache)
            return
        self._populate_all([(p, n) for p, n in InjectorApp._cache
                            if q in n.lower() or q in str(p)])
        self._populate_apps([(p, n, t) for p, n, t in InjectorApp._app_cache
                             if q in n.lower() or q in t.lower() or q in str(p)])

    def _on_tree_select(self, _):
        current_tab = self.notebook.index(self.notebook.select())
        if current_tab == 0:
            sel = self.app_tree.selection()
        else:
            sel = self.tree.selection()
        if sel:
            self.selected_pid = int(sel[0][1:])
        else:
            self.selected_pid = None

    def _on_tab_changed(self, _):
        current_tab = self.notebook.index(self.notebook.select())
        if current_tab == 0:
            for item in self.tree.selection():
                self.tree.selection_remove(item)
        else:
            for item in self.app_tree.selection():
                self.app_tree.selection_remove(item)
        self.selected_pid = None

    def _browse(self):
        path = filedialog.askopenfilename(
            title="Select shared object (.so)",
            filetypes=[("Shared objects", "*.so"),
                       ("Shared objects", "*.so.*"),
                       ("All files", "*.*")])
        if path:
            self.dll_path.set(path)
            self._write_log(f"Selected: {os.path.basename(path)}", "dim")
            if path.lower().endswith(".dll"):
                self._write_log(
                    "That looks like a Windows .dll — Linux needs a .so.", "warn")

    def _inject(self):
        if self.selected_pid is None:
            self._write_log("No process selected.", "warn")
            return
        dll = self.dll_path.get().strip()
        if not dll:
            self._write_log("No .so path specified.", "warn")
            return
        if dll.lower().endswith(".dll"):
            self._write_log(
                "Windows .dll cannot be injected on Linux — build a .so.", "warn")

        pid = self.selected_pid
        self._write_log(f"Injecting into PID {pid} …")
        self.inject_btn.configure(state="disabled")

        def worker():
            result = inject_so(pid, dll)
            self.root.after(0, lambda: self._inject_done(result))

        threading.Thread(target=worker, daemon=True).start()

    def _inject_done(self, result):
        t = TEXTS[self.lang]
        self.inject_btn.configure(state="normal")
        if result.startswith(OK):
            self._write_log(result, "ok")
            messagebox.showinfo(t["ok_title"], t["ok_msg"])
        else:
            self._write_log(result, "err")
            messagebox.showerror(t["fail_title"], t["fail_msg"])


# ─── Main ────────────────────────────────────────────────────────────
def main():
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

    # className sets WM_CLASS so launchers/Hyprland can match the window
    # (e.g. `windowrule = float, class:(GloryInjector)`).
    root = tk.Tk(className="GloryInjector")
    app = InjectorApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
