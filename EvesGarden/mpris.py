"""MPRIS2: the desktop's own idea of a media player.

The Windows build called RegisterHotKey to claim the four media keys for
itself. That is the wrong shape on Linux, and not because the API is missing
-- it is that on Linux the media keys are not the application's to claim.
The compositor owns them, the user has already bound them, and two players
that both grabbed XF86AudioPlay would fight over which one got it.

What the desktop expects instead is that a player announce itself on D-Bus
under org.mpris.MediaPlayer2, and the same registration then does far more
than the hotkeys ever did:

  * media keys work, routed by the compositor to whichever player is active,
    with no grab and no conflict
  * `playerctl play-pause` and any script built on it work
  * waybar, GNOME's panel, KDE's media applet and the lock screen show the
    track, the artwork and working transport buttons
  * headset and keyboard buttons that go through the desktop arrive too

So this is not a port of media_keys.py. It replaces four global hotkeys with
the interface that makes the player a citizen of the desktop.

Everything is best-effort: no session bus (a TTY, a container, a stripped
session) simply means no MPRIS, never a failure to start.
"""

import asyncio
import os
import threading
from urllib.parse import quote

BUS_NAME = "org.mpris.MediaPlayer2.evesgarden"
OBJECT_PATH = "/org/mpris/MediaPlayer2"
NO_TRACK = "/org/mpris/MediaPlayer2/TrackList/NoTrack"

# MPRIS speaks microseconds; the player speaks seconds.
USEC = 1_000_000


def _available():
    try:
        import dbus_fast  # noqa: F401
    except ImportError:
        return False
    return bool(os.environ.get("DBUS_SESSION_BUS_ADDRESS")
                or os.path.exists(
                    os.path.join(os.environ.get("XDG_RUNTIME_DIR", ""), "bus")))


class MprisService:
    """Publishes the player on D-Bus and calls back when the desktop asks.

    `on_action(name, value=None)` is called from the D-Bus thread with one of
    "play_pause", "play", "pause", "next", "prev", "stop", "raise", "quit",
    "seek" (value: relative seconds), "set_position" (value: absolute
    seconds), "volume" (value: 0..1), "shuffle" (value: bool) or "loop"
    (value: "None"/"Track"/"Playlist"). Hand back to the UI thread yourself.

    `position_provider` is called for the Position property, which MPRIS
    reads on demand rather than being told -- a panel asking every second
    must get the real playhead, not the value from the last track change.
    """

    def __init__(self, on_action, position_provider=None,
                 identity="Eve's Garden", desktop_entry="evesgarden"):
        self.on_action = on_action
        self.position_provider = position_provider or (lambda: 0.0)
        self.identity = identity
        self.desktop_entry = desktop_entry

        self._loop = None
        self._thread = None
        self._bus = None
        self._root = None
        self._player = None
        self._ready = threading.Event()
        self._started = False
        self._track_serial = 0

    @property
    def active(self):
        return self._started

    # ------------------------------------------------------------- lifecycle

    def start(self):
        if not _available():
            return False
        self._thread = threading.Thread(target=self._run, name="mpris",
                                        daemon=True)
        self._thread.start()
        # Long enough for a bus connection on a loaded machine, short enough
        # that a broken session does not hold up the window appearing.
        self._ready.wait(timeout=3.0)
        return self._started

    def stop(self):
        loop, self._loop = self._loop, None
        if loop is None:
            return
        try:
            loop.call_soon_threadsafe(loop.stop)
        except RuntimeError:
            pass
        thread = self._thread
        if thread and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout=1.5)
        self._thread = None
        self._started = False

    def _run(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self._loop = loop
        try:
            try:
                loop.run_until_complete(self._connect())
            finally:
                # Released whether the connection worked or not: start() is
                # waiting on it, and a session with no bus must not hold the
                # window up for the full timeout before it gives in.
                self._ready.set()
            loop.run_forever()
        except Exception:
            pass
        finally:
            self._loop = None
            self._started = False
            try:
                loop.run_until_complete(loop.shutdown_asyncgens())
            except Exception:
                pass
            loop.close()

    async def _connect(self):
        from dbus_fast.aio import MessageBus
        from dbus_fast import BusType

        self._bus = await MessageBus(bus_type=BusType.SESSION).connect()
        self._root = _RootInterface(self)
        self._player = _PlayerInterface(self)
        self._bus.export(OBJECT_PATH, self._root)
        self._bus.export(OBJECT_PATH, self._player)

        # A second copy of the app must not silently steal the first one's
        # name, or the panel would follow whichever won and the other would
        # be invisible. The spec's answer is a unique suffix.
        from dbus_fast import RequestNameReply
        reply = await self._bus.request_name(BUS_NAME)
        if reply not in (RequestNameReply.PRIMARY_OWNER,
                         RequestNameReply.ALREADY_OWNER):
            await self._bus.request_name(f"{BUS_NAME}.instance{os.getpid()}")
        self._started = True

    # ---------------------------------------------------------------- update

    def update(self, *, title=None, artist=None, album=None, art_url=None,
               duration=0.0, playing=False, stopped=False, track_path=None,
               can_next=True, can_prev=True, shuffle=False, loop=None,
               volume=None):
        """Tell the desktop what is playing. Safe to call from any thread."""
        if not self._started:
            return
        state = "Stopped" if stopped else ("Playing" if playing else "Paused")
        payload = dict(
            title=title, artist=artist, album=album, art_url=art_url,
            duration=duration, state=state, track_path=track_path,
            can_next=can_next, can_prev=can_prev, shuffle=shuffle,
            loop=loop, volume=volume,
        )
        self._call(lambda: self._player.apply(payload))

    def seeked(self, position):
        """Announce a jump. Panels only redraw their scrubber on this."""
        if not self._started:
            return
        self._call(lambda: self._player.Seeked(int(position * USEC)))

    def _call(self, fn):
        loop = self._loop
        if loop is None:
            return
        try:
            loop.call_soon_threadsafe(fn)
        except RuntimeError:
            pass

    def next_track_path(self):
        """A fresh D-Bus object path for a track.

        Panels key their state off this. Reusing one path for every track
        makes a change of song look like the same song still playing, and
        artwork and titles stop updating.
        """
        self._track_serial += 1
        return f"/org/mpris/MediaPlayer2/Track/{self._track_serial}"

    def _fire(self, name, value=None):
        try:
            self.on_action(name, value)
        except Exception:
            pass


# --------------------------------------------------------------- interfaces
# Imported lazily: dbus_fast's ServiceInterface base class must exist before
# the class body runs, and this module has to be importable without it.

def _build_interfaces():
    from dbus_fast import PropertyAccess, Variant
    from dbus_fast.service import ServiceInterface, dbus_property, method, signal

    class Root(ServiceInterface):
        def __init__(self, service):
            super().__init__("org.mpris.MediaPlayer2")
            self._service = service

        @method()
        def Raise(self):
            self._service._fire("raise")

        @method()
        def Quit(self):
            self._service._fire("quit")

        @dbus_property(access=PropertyAccess.READ)
        def CanQuit(self) -> "b":
            return True

        @dbus_property(access=PropertyAccess.READ)
        def CanRaise(self) -> "b":
            return True

        @dbus_property(access=PropertyAccess.READ)
        def HasTrackList(self) -> "b":
            return False

        @dbus_property(access=PropertyAccess.READ)
        def Identity(self) -> "s":
            return self._service.identity

        @dbus_property(access=PropertyAccess.READ)
        def DesktopEntry(self) -> "s":
            # Without this a panel has no .desktop file to read, so it shows
            # a generic placeholder instead of the app's icon.
            return self._service.desktop_entry

        @dbus_property(access=PropertyAccess.READ)
        def SupportedUriSchemes(self) -> "as":
            return ["file"]

        @dbus_property(access=PropertyAccess.READ)
        def SupportedMimeTypes(self) -> "as":
            return ["audio/mpeg", "audio/flac", "audio/mp4", "audio/ogg",
                    "audio/opus", "audio/x-wav", "audio/x-m4a"]

    class Player(ServiceInterface):
        def __init__(self, service):
            super().__init__("org.mpris.MediaPlayer2.Player")
            self._service = service
            self._status = "Stopped"
            self._metadata = {"mpris:trackid": Variant("o", NO_TRACK)}
            self._can_next = False
            self._can_prev = False
            self._volume = 1.0
            self._shuffle = False
            self._loop = "None"

        # ----------------------------------------------------------- methods

        @method()
        def Next(self):
            self._service._fire("next")

        @method()
        def Previous(self):
            self._service._fire("prev")

        @method()
        def Pause(self):
            self._service._fire("pause")

        @method()
        def PlayPause(self):
            self._service._fire("play_pause")

        @method()
        def Stop(self):
            self._service._fire("stop")

        @method()
        def Play(self):
            self._service._fire("play")

        @method()
        def Seek(self, offset: "x"):
            self._service._fire("seek", offset / USEC)

        @method()
        def SetPosition(self, track_id: "o", position: "x"):
            # The track id guards against a panel seeking the track that was
            # playing when its button was drawn, after the song has changed.
            current = self._metadata.get("mpris:trackid")
            if current is not None and current.value != track_id:
                return
            self._service._fire("set_position", position / USEC)

        @method()
        def OpenUri(self, uri: "s"):
            self._service._fire("open_uri", uri)

        @signal()
        def Seeked(self, position: "x") -> "x":
            return position

        # -------------------------------------------------------- properties

        @dbus_property(access=PropertyAccess.READ)
        def PlaybackStatus(self) -> "s":
            return self._status

        @dbus_property()
        def LoopStatus(self) -> "s":
            return self._loop

        @LoopStatus.setter
        def LoopStatus(self, value: "s"):
            self._service._fire("loop", value)

        @dbus_property(access=PropertyAccess.READ)
        def Rate(self) -> "d":
            return 1.0

        @dbus_property()
        def Shuffle(self) -> "b":
            return self._shuffle

        @Shuffle.setter
        def Shuffle(self, value: "b"):
            self._service._fire("shuffle", bool(value))

        @dbus_property(access=PropertyAccess.READ)
        def Metadata(self) -> "a{sv}":
            return self._metadata

        @dbus_property()
        def Volume(self) -> "d":
            return self._volume

        @Volume.setter
        def Volume(self, value: "d"):
            self._service._fire("volume", max(0.0, min(1.0, float(value))))

        @dbus_property(access=PropertyAccess.READ)
        def Position(self) -> "x":
            try:
                return int(self._service.position_provider() * USEC)
            except Exception:
                return 0

        @dbus_property(access=PropertyAccess.READ)
        def MinimumRate(self) -> "d":
            return 1.0

        @dbus_property(access=PropertyAccess.READ)
        def MaximumRate(self) -> "d":
            return 1.0

        @dbus_property(access=PropertyAccess.READ)
        def CanGoNext(self) -> "b":
            return self._can_next

        @dbus_property(access=PropertyAccess.READ)
        def CanGoPrevious(self) -> "b":
            return self._can_prev

        @dbus_property(access=PropertyAccess.READ)
        def CanPlay(self) -> "b":
            return True

        @dbus_property(access=PropertyAccess.READ)
        def CanPause(self) -> "b":
            return True

        @dbus_property(access=PropertyAccess.READ)
        def CanSeek(self) -> "b":
            return True

        @dbus_property(access=PropertyAccess.READ)
        def CanControl(self) -> "b":
            return True

        # ------------------------------------------------------------ update

        def apply(self, payload):
            """Merge new state in and emit PropertiesChanged for what moved.

            Only the changed keys are announced. A panel redraws on every
            signal, so sending the full set once a second -- which is what
            "just emit everything" amounts to while a track plays -- makes
            the applet flicker.
            """
            # emit_properties_changed takes plain Python values and wraps
            # them using each property's own declared signature; handing it
            # a Variant makes it try to wrap the wrapper.
            changed = {}

            if payload["state"] != self._status:
                self._status = payload["state"]
                changed["PlaybackStatus"] = self._status

            metadata = self._build_metadata(payload)
            if metadata != self._metadata:
                self._metadata = metadata
                changed["Metadata"] = metadata

            if bool(payload["can_next"]) != self._can_next:
                self._can_next = bool(payload["can_next"])
                changed["CanGoNext"] = self._can_next

            if bool(payload["can_prev"]) != self._can_prev:
                self._can_prev = bool(payload["can_prev"])
                changed["CanGoPrevious"] = self._can_prev

            if bool(payload["shuffle"]) != self._shuffle:
                self._shuffle = bool(payload["shuffle"])
                changed["Shuffle"] = self._shuffle

            if payload["loop"] and payload["loop"] != self._loop:
                self._loop = payload["loop"]
                changed["LoopStatus"] = self._loop

            volume = payload.get("volume")
            if volume is not None and abs(volume - self._volume) > 0.001:
                self._volume = float(volume)
                changed["Volume"] = self._volume

            if changed:
                self.emit_properties_changed(changed)

        def _build_metadata(self, payload):
            if payload["state"] == "Stopped" and not payload.get("title"):
                return {"mpris:trackid": Variant("o", NO_TRACK)}

            track = payload.get("track_path") or NO_TRACK
            data = {
                "mpris:trackid": Variant("o", track),
                "xesam:title": Variant("s", payload.get("title") or "Unknown"),
            }
            artist = payload.get("artist")
            if artist:
                data["xesam:artist"] = Variant("as", [artist])
            album = payload.get("album")
            if album:
                data["xesam:album"] = Variant("s", album)
            duration = payload.get("duration") or 0.0
            if duration > 0:
                data["mpris:length"] = Variant("x", int(duration * USEC))
            art = payload.get("art_url")
            if art:
                data["mpris:artUrl"] = Variant("s", art)
            return data

    return Root, Player


class _Lazy:
    """Defer building the interface classes until dbus_fast is known present."""

    _built = None

    @classmethod
    def get(cls):
        if cls._built is None:
            cls._built = _build_interfaces()
        return cls._built


def _RootInterface(service):
    return _Lazy.get()[0](service)


def _PlayerInterface(service):
    return _Lazy.get()[1](service)


def file_url(path):
    """A file:// URL for artwork, which is how MPRIS carries a local image."""
    if not path:
        return None
    return "file://" + quote(os.path.abspath(path))
