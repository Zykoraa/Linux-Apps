"""Showing a track in the user's file manager.

The Windows build ran `explorer /select,<path>`, which opens the containing
folder with the file highlighted. Linux has no single file manager to call,
but it does have an agreed way to ask whichever one is installed: the
org.freedesktop.FileManager1 D-Bus interface, which Nautilus, Dolphin, Nemo,
Thunar, PCManFM and Caja all implement, and whose ShowItems method does
exactly what /select, does -- opens the folder with the file selected.

`xdg-open` is the fallback, and it is a real step down: it can only open the
directory, so the file is not selected, and in a folder of four hundred
tracks that is a meaningfully worse answer. It is still much better than
nothing, which is what the alternative would be on a machine with no
FileManager1.
"""

import os
import shutil
import subprocess

BUS_NAME = "org.freedesktop.FileManager1"
OBJECT_PATH = "/org/freedesktop/FileManager1"


def reveal(path):
    """Open the file manager with `path` selected. Returns False if it could
    not be done at all, so the caller can say so."""
    path = os.path.abspath(path)
    if not os.path.exists(path):
        return False
    if _show_items(path):
        return True
    return _open_folder(os.path.dirname(path))


def _show_items(path):
    """Ask the desktop's file manager to select the file."""
    from urllib.parse import quote

    uri = "file://" + quote(path)
    try:
        import dbus_fast  # noqa: F401
    except ImportError:
        return _show_items_via_gdbus(uri)
    return _show_items_via_dbus_fast(uri) or _show_items_via_gdbus(uri)


def _show_items_via_dbus_fast(uri):
    import asyncio

    async def call():
        from dbus_fast.aio import MessageBus
        from dbus_fast import BusType, Message, MessageType

        bus = await MessageBus(bus_type=BusType.SESSION).connect()
        try:
            reply = await bus.call(Message(
                destination=BUS_NAME,
                path=OBJECT_PATH,
                interface=BUS_NAME,
                member="ShowItems",
                signature="ass",
                # The second argument is a startup id, which we have none of.
                body=[[uri], ""],
            ))
            return reply is not None and reply.message_type != MessageType.ERROR
        finally:
            bus.disconnect()

    try:
        # Its own loop: this is called from the Tk thread, which has none, and
        # the MPRIS service's loop belongs to another thread.
        return asyncio.run(asyncio.wait_for(call(), timeout=5.0))
    except Exception:
        return False


def _show_items_via_gdbus(uri):
    """The same call through gdbus, for a machine without dbus-fast."""
    binary = shutil.which("gdbus")
    if not binary:
        return False
    try:
        result = subprocess.run(
            [binary, "call", "--session", "--dest", BUS_NAME,
             "--object-path", OBJECT_PATH,
             "--method", f"{BUS_NAME}.ShowItems", f"['{uri}']", ""],
            capture_output=True, timeout=5)
        return result.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _open_folder(folder):
    if not folder or not os.path.isdir(folder):
        return False
    opener = shutil.which("xdg-open")
    if not opener:
        return False
    try:
        # Detached, and with its output discarded: xdg-open on some desktops
        # is a shell script that stays alive as long as the file manager it
        # started, and a pipe nobody reads eventually blocks it.
        subprocess.Popen([opener, folder],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return True
    except OSError:
        return False
