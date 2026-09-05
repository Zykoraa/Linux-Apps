"""Finding ffmpeg and ffprobe.

The Windows build shipped its own copies in `bin/` and pointed everything at
`bin\\ffmpeg.exe`, because Windows has no package manager to get them from and
no PATH convention that would find them.

Linux does. ffmpeg is a package on every distribution, it is already on PATH,
and it is kept patched by the same updates as everything else -- so the
bundled copy is the wrong default here: it would be a second, stale ffmpeg
that never gets a security update.

A bundled copy is still honoured when one is present, so a self-contained
PyInstaller build keeps working, but PATH is what is used otherwise.
"""

import os
import shutil
import sys


def _bundled_dir():
    """`bin/` beside the app, or inside the PyInstaller bundle."""
    base = (sys._MEIPASS if getattr(sys, "frozen", False)
            else os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "bin")


def _find(name):
    bundled = os.path.join(_bundled_dir(), name)
    if os.path.isfile(bundled) and os.access(bundled, os.X_OK):
        return bundled
    return shutil.which(name)


def ffmpeg():
    """Path to ffmpeg, or None. Callers must handle None -- it is installable."""
    return _find("ffmpeg")


def ffprobe():
    return _find("ffprobe")


def available():
    return bool(ffmpeg())


MISSING_MESSAGE = (
    "ffmpeg was not found on your PATH. It decodes and converts every format "
    "this app handles, so downloads and playback of anything but WAV need it.\n"
    "\n"
    "    Arch / CachyOS   sudo pacman -S ffmpeg\n"
    "    Debian / Ubuntu  sudo apt install ffmpeg\n"
    "    Fedora           sudo dnf install ffmpeg\n"
    "    openSUSE         sudo zypper install ffmpeg"
)


def configure_pydub():
    """Point pydub at whichever ffmpeg we found.

    pydub looks these up itself, but only once at import time and only on
    PATH -- so a bundled build would not find its own copy without this.
    """
    try:
        from pydub import AudioSegment
    except ImportError:
        return False

    found = ffmpeg()
    probe = ffprobe()
    if not found:
        return False

    AudioSegment.converter = found
    if probe:
        AudioSegment.ffprobe = probe

    # A bundled build ships ffprobe next to ffmpeg; make sure anything that
    # shells out by bare name (yt-dlp, say) can find them too.
    directory = os.path.dirname(found)
    path = os.environ.get("PATH", "")
    if directory and directory not in path.split(os.pathsep):
        os.environ["PATH"] = f"{path}{os.pathsep}{directory}" if path else directory
    return True
