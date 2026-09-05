"""Send files to the desktop trash instead of deleting them outright.

Anything that removes a user's music should be undoable. On Windows that was
the Recycle Bin, reached through SHFileOperationW. Linux has the same idea in
the freedesktop.org Trash specification, which every file manager -- Nautilus,
Dolphin, Thunar, Nemo, PCManFM -- reads and writes, so a file trashed here
turns up in the trash they already use and restores from it.

The spec is implemented directly rather than shelling out to `gio trash`,
because gio is a GNOME/GLib dependency this app otherwise does not have, and
the format is a directory and a small ini file.

Two rules in it are what make it more than "move the file somewhere else":

  Trash must not cross a filesystem. Moving a file from a mounted drive into
  the trash under $HOME would be a copy of the whole file, and would then
  free no space on the drive it came from. Each filesystem keeps its own
  trash at its top directory, and that is where a file from it goes.

  The original path must be recorded. A `.trashinfo` file beside each trashed
  item says where it came from and when it went, and without one a file
  manager has nowhere to restore it to and will show it as an orphan.
"""

import errno
import os
import time
from urllib.parse import quote

import xdg_paths

INFO_SUFFIX = ".trashinfo"


def available():
    """Whether trashing is possible at all -- i.e. we can write a home trash."""
    try:
        files, info = _home_trash()
        return bool(files and info)
    except OSError:
        return False


def send_to_recycle_bin(paths):
    """Trash every path given. Returns (trashed, failed).

    Named for the Windows call it replaces so the callers did not have to
    change. Nothing is ever deleted outright: a file that cannot be trashed
    is reported as failed and left where it is.

    That is a deliberate difference from the Windows build, which fell back
    to os.remove when the shell refused. On Windows that fallback was rare
    and covered network shares; here it would fire on any read-only or
    unusual mount, and silently turn "move 300 duplicates to the trash" into
    "erase 300 files". A failure the user can retry is better than a
    deletion they cannot undo.
    """
    paths = [os.path.abspath(p) for p in paths if p and os.path.lexists(p)]
    if not paths:
        return [], []

    trashed, failed = [], []
    for path in paths:
        try:
            _trash_one(path)
            trashed.append(path)
        except OSError:
            failed.append(path)
    return trashed, failed


# ------------------------------------------------------------------ internals

def _home_trash():
    """$XDG_DATA_HOME/Trash, created if missing. Returns (files, info)."""
    root = os.path.join(
        xdg_paths._base("XDG_DATA_HOME", "~/.local/share"), "Trash")
    files = os.path.join(root, "files")
    info = os.path.join(root, "info")
    os.makedirs(files, exist_ok=True)
    os.makedirs(info, exist_ok=True)
    return files, info


def _mount_point(path):
    """The top directory of the filesystem `path` sits on.

    Walks up by device number rather than trusting os.path.ismount alone, so
    bind mounts and btrfs subvolumes -- which share a device with their
    parent -- do not get treated as separate filesystems with their own
    trash directory.
    """
    path = os.path.abspath(path)
    try:
        device = os.lstat(path).st_dev
    except OSError:
        return "/"
    while path != "/":
        parent = os.path.dirname(path)
        try:
            if os.lstat(parent).st_dev != device:
                return path
        except OSError:
            return path
        path = parent
    return "/"


def _volume_trash(top):
    """The trash for a filesystem mounted at `top`, per the spec's two forms.

    $top/.Trash is the administrator-provided one and must be checked first,
    but only counts if it is a real directory with the sticky bit set -- an
    unsticky, world-writable .Trash on a shared volume would let one user
    replace another's trashed files, which is why the spec requires it.
    """
    uid = os.getuid()
    shared = os.path.join(top, ".Trash")
    try:
        stat = os.lstat(shared)
        if (not os.path.islink(shared) and os.path.isdir(shared)
                and stat.st_mode & 0o1000):
            return _make_trash(os.path.join(shared, str(uid)))
    except OSError:
        pass

    # Otherwise the per-user one, which we may create ourselves.
    return _make_trash(os.path.join(top, f".Trash-{uid}"))


def _make_trash(root):
    files = os.path.join(root, "files")
    info = os.path.join(root, "info")
    os.makedirs(files, exist_ok=True)
    os.makedirs(info, exist_ok=True)
    return files, info


def _trash_dirs_for(path):
    """Where `path` should go, and what its recorded Path is relative to.

    Returns (files_dir, info_dir, relative_to). `relative_to` is the volume
    top directory when the file is going to a volume trash -- the spec says
    the recorded path is relative to it there, so the trash still restores
    correctly after the drive is remounted somewhere else.
    """
    home_files, home_info = _home_trash()
    parent = os.path.dirname(path)
    try:
        same = os.lstat(parent).st_dev == os.lstat(home_files).st_dev
    except OSError:
        same = False
    if same:
        return home_files, home_info, None

    top = _mount_point(path)
    files, info = _volume_trash(top)
    return files, info, top


def _reserve(info_dir, name):
    """Claim a name in the trash by creating its info file exclusively.

    Two files called "track.mp3" from different folders can be trashed in the
    same second, and the loser of that race must not overwrite the winner.
    O_EXCL makes the claim atomic: whoever creates the .trashinfo owns the
    name, and the other tries the next one.
    """
    stem, ext = os.path.splitext(name)
    for attempt in range(10000):
        candidate = name if attempt == 0 else f"{stem}.{attempt}{ext}"
        target = os.path.join(info_dir, candidate + INFO_SUFFIX)
        try:
            fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            return candidate, fd
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise
    raise OSError(errno.EEXIST, "no free name in the trash", name)


def _trash_one(path):
    files_dir, info_dir, relative_to = _trash_dirs_for(path)

    recorded = path
    if relative_to:
        recorded = os.path.relpath(path, relative_to)

    name, fd = _reserve(info_dir, os.path.basename(path))
    info_path = os.path.join(info_dir, name + INFO_SUFFIX)
    try:
        # The date is local time with no zone, which is what the spec asks
        # for and what every file manager displays.
        body = (
            "[Trash Info]\n"
            f"Path={quote(recorded)}\n"
            f"DeletionDate={time.strftime('%Y-%m-%dT%H:%M:%S')}\n"
        )
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(body)
    except OSError:
        _unlink(info_path)
        raise

    try:
        os.rename(path, os.path.join(files_dir, name))
    except OSError:
        # The info file is a reservation, not a record, until the move
        # succeeds. Leaving it behind would show an entry in every file
        # manager for a file that is still in the library.
        _unlink(info_path)
        raise


def _unlink(path):
    try:
        os.unlink(path)
    except OSError:
        pass
