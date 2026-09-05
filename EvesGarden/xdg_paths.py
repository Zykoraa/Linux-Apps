"""Where the app keeps its things, per the XDG Base Directory spec.

The Windows build had one folder for everything -- `%LOCALAPPDATA%\\EvesGarden`
-- and, when it could write beside itself, dropped the config there instead.
Linux separates the four kinds of state, and the separation is worth keeping:

  config  ~/.config/evesgarden      credentials, settings.json
  data    ~/.local/share/evesgarden the library index
  cache   ~/.cache/evesgarden       the Spotify token cache, cover art
  state   ~/.local/state/evesgarden logs

The point is not tidiness. `~/.config` is what people back up and sync;
`~/.cache` is what a disk-cleanup tool is entitled to delete without asking.
Putting the OAuth token cache in `~/.config` means a cleaner takes your
settings with it, and putting settings in `~/.cache` means it takes those.

Every path here honours its XDG_* environment variable, so a machine that
relocates these -- or a test that points them at a temporary directory --
gets what it asked for rather than a hardcoded `~/.config`.
"""

import os
import shutil
import subprocess

APP = "evesgarden"


def _base(var, default):
    """An XDG base directory: the variable if it is set to an absolute path.

    The spec is explicit that a relative value is invalid and must be
    ignored, which matters because an empty XDG_CONFIG_HOME= in a shell
    profile is common and would otherwise resolve to the process working
    directory.
    """
    value = os.environ.get(var, "").strip()
    if value and os.path.isabs(value):
        return value
    return os.path.expanduser(default)


def config_dir():
    return os.path.join(_base("XDG_CONFIG_HOME", "~/.config"), APP)


def data_dir():
    return os.path.join(_base("XDG_DATA_HOME", "~/.local/share"), APP)


def cache_dir():
    return os.path.join(_base("XDG_CACHE_HOME", "~/.cache"), APP)


def state_dir():
    return os.path.join(_base("XDG_STATE_HOME", "~/.local/state"), APP)


def runtime_dir():
    """For sockets and pid files. Falls back to the cache, as the spec allows."""
    value = os.environ.get("XDG_RUNTIME_DIR", "").strip()
    if value and os.path.isdir(value):
        return os.path.join(value, APP)
    return cache_dir()


def ensure(path):
    """Create a directory and hand it back, or hand back $HOME if we cannot.

    Called on the startup path, where raising means no window ever appears.
    """
    try:
        os.makedirs(path, exist_ok=True)
        return path
    except OSError:
        return os.path.expanduser("~")


def music_dir():
    """The user's music folder, as their desktop defines it.

    `~/Music` is only the English default. xdg-user-dirs writes the real one
    to ~/.config/user-dirs.dirs, and on a French or German install it is
    `~/Musique` or `~/Musik` -- so a hardcoded ~/Music would quietly create a
    second, empty music folder beside the one they already use.
    """
    found = _xdg_user_dir("MUSIC")
    if found:
        return found
    return os.path.expanduser("~/Music")


def _xdg_user_dir(name):
    """Read one entry from user-dirs.dirs, preferring the tool that owns it."""
    binary = shutil.which("xdg-user-dir")
    if binary:
        try:
            out = subprocess.run([binary, name], capture_output=True,
                                 text=True, timeout=5).stdout.strip()
            # It echoes $HOME when the directory is not configured, which is
            # not an answer -- treat that as "not set" rather than putting a
            # music library loose in the home directory.
            if out and os.path.abspath(out) != os.path.abspath(
                    os.path.expanduser("~")):
                return out
        except (OSError, subprocess.SubprocessError):
            pass

    # No xdg-user-dirs installed: parse the file it would have read.
    path = os.path.join(_base("XDG_CONFIG_HOME", "~/.config"), "user-dirs.dirs")
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                key, _, value = line.partition("=")
                if key.strip() != f"XDG_{name}_DIR":
                    continue
                value = value.strip().strip('"')
                if value.startswith("$HOME"):
                    value = os.path.expanduser("~") + value[5:]
                if value and value != os.path.expanduser("~"):
                    return value
    except OSError:
        pass
    return None


def library_dir():
    """Where downloads land: <music>/Eve's Garden."""
    return os.path.join(music_dir(), "Eve's Garden")
