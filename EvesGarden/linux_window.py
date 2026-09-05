"""Frameless-but-managed windows on X11.

The Windows build drew its own title bar and got there with
overrideredirect(True), then spent a second call putting back what that
takes away -- WS_THICKFRAME and the maximise box, so the shell would still
snap and tile it.

overrideredirect means something stronger on X11 than it does on Windows. It
tells the window manager to leave the window alone entirely: no entry in the
taskbar, no Alt-Tab, no workspace, no keyboard focus handed over by the WM,
and under a tiling compositor such as Hyprland it is not a window the
compositor will ever place. It is the flag menus and tooltips use. A player
you cannot Alt-Tab back to is not a working application, so the port does
not use it.

The X11 way to get the same look is to stay a normal managed window and ask
the window manager not to draw a frame, through _MOTIF_WM_HINTS -- a
convention from 1989 that every current window manager still honours. The
window then keeps everything: taskbar, Alt-Tab, workspaces, tiling.

Dragging is handed to the window manager with _NET_WM_MOVERESIZE, which is
the X11 counterpart of the WM_NCLBUTTONDOWN trick the Windows build had to
abandon. It is safe here for the reason that one was not: it is an
asynchronous ClientMessage, not a call that blocks inside a modal loop with
the GIL released, so nothing dispatches back into Tk while Python is not
holding the interpreter. Handing the drag over is also what gets the window
manager's own edge snapping, half-tiling and cross-monitor handling, instead
of this app reimplementing them badly.

Everything degrades: with no X connection, or a window manager that supports
none of this, every method returns False and the caller keeps its own
geometry-based fallback.
"""

import os

# _NET_WM_MOVERESIZE directions, from the EWMH spec.
_SIZE_TOPLEFT, _SIZE_TOP, _SIZE_TOPRIGHT, _SIZE_RIGHT = 0, 1, 2, 3
_SIZE_BOTTOMRIGHT, _SIZE_BOTTOM, _SIZE_BOTTOMLEFT, _SIZE_LEFT = 4, 5, 6, 7
_MOVE = 8
_CANCEL = 11

_EDGE_DIRECTIONS = {
    "n": _SIZE_TOP, "s": _SIZE_BOTTOM, "e": _SIZE_RIGHT, "w": _SIZE_LEFT,
    "ne": _SIZE_TOPRIGHT, "nw": _SIZE_TOPLEFT,
    "se": _SIZE_BOTTOMRIGHT, "sw": _SIZE_BOTTOMLEFT,
}

# _NET_WM_STATE actions.
_STATE_REMOVE, _STATE_ADD, _STATE_TOGGLE = 0, 1, 2

# The application ID everything else keys off: the .desktop file name, the
# WM_CLASS the compositor matches rules against, and the MPRIS DesktopEntry.
APP_ID = "evesgarden"
WM_CLASS_NAME = "evesgarden"
WM_CLASS_CLASS = "EvesGarden"


class LinuxWindow:
    """X11 window-manager cooperation for one Tk toplevel."""

    def __init__(self, window):
        self.window = window
        self._display = None
        self._client = None
        self._root = None
        self._atoms = {}
        self._supported = None
        self._connect()

    # ------------------------------------------------------------ connection

    def _connect(self):
        if not os.environ.get("DISPLAY"):
            return
        try:
            from Xlib import display, X
        except ImportError:
            return
        try:
            self._display = display.Display()
            self._root = self._display.screen().root
            self._client = self._find_client_window()
        except Exception:
            self._display = None
            self._client = None

    def _find_client_window(self):
        """The window the window manager actually manages.

        Not winfo_id(). Tk puts every toplevel inside a wrapper window and
        it is the wrapper the WM adopts -- so winfo_id() is one level too
        deep, and a property set on it is read by nobody. The managed window
        is the first one up the tree carrying WM_STATE, which is precisely
        what "the WM has adopted this" means.
        """
        from Xlib import X

        try:
            self.window.update_idletasks()
            current = self._display.create_resource_object(
                "window", self.window.winfo_id())
        except Exception:
            return None

        wm_state = self._display.intern_atom("WM_STATE")
        for _ in range(8):
            try:
                if current.get_property(wm_state, X.AnyPropertyType, 0, 4):
                    return current
                tree = current.query_tree()
            except Exception:
                return current
            if tree.parent is None or tree.parent.id == tree.root.id:
                return current
            current = tree.parent
        return current

    @property
    def available(self):
        return self._client is not None

    def _atom(self, name):
        if name not in self._atoms:
            self._atoms[name] = self._display.intern_atom(name)
        return self._atoms[name]

    def supports(self, name):
        """Whether the window manager advertises a hint in _NET_SUPPORTED."""
        if not self.available:
            return False
        if self._supported is None:
            from Xlib import X
            try:
                prop = self._root.get_full_property(
                    self._atom("_NET_SUPPORTED"), X.AnyPropertyType)
                self._supported = {self._display.get_atom_name(a)
                                   for a in (prop.value if prop else [])}
            except Exception:
                self._supported = set()
        return name in self._supported

    # ----------------------------------------------------------- decorations

    def undecorate(self):
        """Ask the window manager not to draw a title bar or border."""
        if not self.available:
            return False
        from Xlib import Xatom
        try:
            atom = self._atom("_MOTIF_WM_HINTS")
            # flags=MWM_HINTS_DECORATIONS, functions, decorations=0,
            # input_mode, status.
            self._client.change_property(atom, atom, 32, [2, 0, 0, 0, 0])
            self._display.flush()
            return True
        except Exception:
            return False

    def set_class(self, name=WM_CLASS_NAME, cls=WM_CLASS_CLASS):
        """Set WM_CLASS, which is how everything else recognises the window.

        Tk names it after the script -- "gui" -- so the desktop file never
        matched, the taskbar showed a stock icon, and a Hyprland
        `windowrule ... class:` could not target it.
        """
        if not self.available:
            return False
        from Xlib import Xatom
        try:
            self._client.change_property(
                Xatom.WM_CLASS, Xatom.STRING, 8,
                (name + "\0" + cls + "\0").encode("latin-1"))
            self._display.flush()
            return True
        except Exception:
            return False

    # ------------------------------------------------------------ move/resize

    def begin_move(self, x_root, y_root, button=1):
        return self._moveresize(_MOVE, x_root, y_root, button)

    def begin_resize(self, edge, x_root, y_root, button=1):
        direction = _EDGE_DIRECTIONS.get(edge)
        if direction is None:
            return False
        return self._moveresize(direction, x_root, y_root, button)

    def cancel_moveresize(self):
        self._moveresize(_CANCEL, 0, 0, 0)

    def _moveresize(self, direction, x_root, y_root, button):
        if not self.available or not self.supports("_NET_WM_MOVERESIZE"):
            return False
        from Xlib import X, protocol
        try:
            # Tk holds an implicit pointer grab from the button press that
            # started this. The window manager cannot take over the drag
            # while another client owns the pointer, so the grab has to go
            # first or the window simply never moves.
            self._display.ungrab_pointer(X.CurrentTime)
            self._display.flush()

            event = protocol.event.ClientMessage(
                window=self._client,
                client_type=self._atom("_NET_WM_MOVERESIZE"),
                data=(32, [int(x_root), int(y_root), int(direction),
                           int(button), 1]),
            )
            mask = X.SubstructureRedirectMask | X.SubstructureNotifyMask
            self._root.send_event(event, event_mask=mask)
            self._display.flush()
            return True
        except Exception:
            return False

    # ------------------------------------------------------------------ state

    def _wm_state(self, action, *names):
        if not self.available or not self.supports("_NET_WM_STATE"):
            return False
        from Xlib import X, protocol
        try:
            atoms = [self._atom(n) for n in names]
            atoms += [0] * (2 - len(atoms))
            event = protocol.event.ClientMessage(
                window=self._client,
                client_type=self._atom("_NET_WM_STATE"),
                data=(32, [action, atoms[0], atoms[1], 1, 0]),
            )
            mask = X.SubstructureRedirectMask | X.SubstructureNotifyMask
            self._root.send_event(event, event_mask=mask)
            self._display.flush()
            return True
        except Exception:
            return False

    def toggle_maximize(self):
        return self._wm_state(_STATE_TOGGLE,
                              "_NET_WM_STATE_MAXIMIZED_HORZ",
                              "_NET_WM_STATE_MAXIMIZED_VERT")

    def maximized(self):
        if not self.available:
            return False
        from Xlib import X
        try:
            prop = self._client.get_full_property(
                self._atom("_NET_WM_STATE"), X.AnyPropertyType)
            if not prop:
                return False
            names = {self._display.get_atom_name(a) for a in prop.value}
            return "_NET_WM_STATE_MAXIMIZED_VERT" in names
        except Exception:
            return False

    def activate(self):
        """Raise and focus the window -- what MPRIS Raise() has to do.

        deiconify() alone is not enough: a compositor that follows the EWMH
        focus-stealing rules will show the window without focusing it, and
        clicking Raise in a panel would leave the keyboard somewhere else.
        """
        if not self.available or not self.supports("_NET_ACTIVE_WINDOW"):
            return False
        from Xlib import X, protocol
        try:
            event = protocol.event.ClientMessage(
                window=self._client,
                client_type=self._atom("_NET_ACTIVE_WINDOW"),
                # source=2 (pager): asks to be allowed past the
                # focus-stealing rules, which is what a panel button is.
                data=(32, [2, X.CurrentTime, 0, 0, 0]),
            )
            mask = X.SubstructureRedirectMask | X.SubstructureNotifyMask
            self._root.send_event(event, event_mask=mask)
            self._display.flush()
            return True
        except Exception:
            return False

    # -------------------------------------------------------------- geometry

    def work_area(self):
        """The desktop minus panels and bars, or None if the WM will not say.

        The Windows build read SPI_GETWORKAREA for the same reason: snapping
        to half the screen puts the bottom of the window behind the taskbar
        on any machine that has one. waybar, a GNOME dock and a KDE panel all
        take the same bite out of the screen.
        """
        if not self.available or not self.supports("_NET_WORKAREA"):
            return None
        from Xlib import X
        try:
            desktop = 0
            current = self._root.get_full_property(
                self._atom("_NET_CURRENT_DESKTOP"), X.AnyPropertyType)
            if current and current.value:
                desktop = int(current.value[0])

            prop = self._root.get_full_property(
                self._atom("_NET_WORKAREA"), X.AnyPropertyType)
            if not prop or len(prop.value) < 4:
                return None
            # Four cardinals per desktop: x, y, width, height.
            offset = desktop * 4
            if offset + 4 > len(prop.value):
                offset = 0
            x, y, width, height = prop.value[offset:offset + 4]
            if width <= 0 or height <= 0:
                return None
            return int(x), int(y), int(x + width), int(y + height)
        except Exception:
            return None

    def close(self):
        try:
            if self._display is not None:
                self._display.close()
        except Exception:
            pass
        self._display = None
        self._client = None
