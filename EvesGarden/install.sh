#!/usr/bin/env bash
# Eve's Garden — user-level installer (no root required).
#
#   ./install.sh              install or upgrade
#   ./install.sh --uninstall  remove the app (never your music or settings)
#
# Installs to ~/.local so it appears in wofi/rofi/fuzzel and any app menu.
# The Python libraries go in a virtualenv of the app's own, so nothing is
# installed into the system Python and nothing can conflict with a distro
# package -- which also means Arch's externally-managed-environment refusal
# never comes up.
set -euo pipefail

APP_ID="evesgarden"
SRC="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

SHARE="$HOME/.local/share"
BIN="$HOME/.local/bin"
# ~/.local/lib, not ~/.local/share/evesgarden -- because that is exactly where
# the app keeps the library index (see xdg_paths.py), and an uninstall that
# does `rm -rf` on its own install directory would take the index, the play
# counts and every "liked" mark with it while printing that it had left them
# alone. Program files and user data must not share a directory that anything
# deletes wholesale.
APP_DIR="$HOME/.local/lib/$APP_ID"
VENV="$APP_DIR/venv"
DESKTOP="$SHARE/applications/$APP_ID.desktop"
ICON_ROOT="$SHARE/icons/hicolor"
LAUNCH="$BIN/$APP_ID"

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$SHARE/applications" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$ICON_ROOT" >/dev/null 2>&1 || true
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$APP_DIR"
    rm -f "$DESKTOP" "$LAUNCH"
    find "$ICON_ROOT" -name "$APP_ID.png" -delete 2>/dev/null || true
    refresh_caches
    echo "Removed Eve's Garden."
    echo
    echo "Your music, settings and library index were left alone:"
    echo "  music    ~/Music/Eve's Garden"
    echo "  config   ~/.config/$APP_ID"
    echo "  index    ~/.local/share/$APP_ID   (this one is regenerable)"
    echo "  cache    ~/.cache/$APP_ID"
    echo
    echo "To remove those too:"
    echo "  rm -rf ~/.config/$APP_ID ~/.local/share/$APP_ID ~/.cache/$APP_ID \\"
    echo "         ~/.local/state/$APP_ID"
    exit 0
fi

# --------------------------------------------------------------- sanity check
[ -f "$SRC/gui.py" ] || { echo "error: gui.py must sit next to install.sh" >&2; exit 1; }

# ---------------------------------------------------------------- dependencies
# Named per distribution, because "install portaudio" is not actionable and
# the package names genuinely differ.
missing=()
note() { missing+=("$1"); }

command -v python3 >/dev/null 2>&1 || note "python3"
python3 -c 'import tkinter' >/dev/null 2>&1 || note "tk"
python3 -c 'import venv'    >/dev/null 2>&1 || note "python-venv"
command -v ffmpeg >/dev/null 2>&1 || note "ffmpeg"
# pyaudio has no Linux wheel; pip compiles it against PortAudio's headers.
# Without them the build fails with a bare "portaudio.h: No such file", which
# says nothing about what to install.
#
# pkg-config is the answer where it knows, since a distribution is free to
# put the header wherever it likes; the fixed paths are the fallback for a
# machine with no pkg-config. Each is tested on its own -- `ls a b` exits
# non-zero when either one is missing, so testing them together would report
# PortAudio absent on any machine that has it in only one of the two places,
# which is every machine.
have_portaudio() {
    if command -v pkg-config >/dev/null 2>&1 &&
       pkg-config --exists portaudio-2.0; then
        return 0
    fi
    for header in /usr/include/portaudio.h /usr/local/include/portaudio.h \
                  /usr/include/*/portaudio.h; do
        [ -f "$header" ] && return 0
    done
    return 1
}
have_portaudio || note "portaudio"

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}"
    echo
    echo "Install them first:"
    echo "  Arch / CachyOS   sudo pacman -S python tk python-pipx ffmpeg portaudio base-devel"
    echo "  Debian / Ubuntu  sudo apt install python3 python3-tk python3-venv python3-dev ffmpeg portaudio19-dev build-essential"
    echo "  Fedora           sudo dnf install python3 python3-tkinter python3-devel ffmpeg portaudio-devel gcc"
    echo "  openSUSE         sudo zypper install python3 python3-tk python3-devel ffmpeg portaudio-devel gcc"
    exit 1
fi

# ---------------------------------------------------------------- install files
echo "Installing to $APP_DIR"
mkdir -p "$APP_DIR" "$BIN" "$(dirname "$DESKTOP")"

# Copy the application, but not the development clutter: no tests, no venv
# from a source checkout, no __pycache__ compiled against another Python.
for f in "$SRC"/*.py; do
    install -m 0644 "$f" "$APP_DIR/"
done
install -m 0644 "$SRC/requirements.txt" "$APP_DIR/"
rm -rf "$APP_DIR/assets"
cp -r "$SRC/assets" "$APP_DIR/assets"
find "$APP_DIR" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true

# ------------------------------------------------------------------- the venv
if [ ! -x "$VENV/bin/python" ]; then
    echo "Creating a virtualenv"
    python3 -m venv "$VENV"
fi
echo "Installing Python libraries (this compiles pyaudio, so it takes a minute)"
"$VENV/bin/python" -m pip install --upgrade pip --quiet
"$VENV/bin/python" -m pip install -r "$APP_DIR/requirements.txt" --quiet

# ------------------------------------------------------------------- launcher
cat > "$LAUNCH" <<EOF
#!/usr/bin/env bash
# Eve's Garden. Written by install.sh -- edit the source, not this.
exec "$VENV/bin/python" "$APP_DIR/gui.py" "\$@"
EOF
chmod 0755 "$LAUNCH"

# ---------------------------------------------------------------------- icons
# Every size, laid out as the hicolor theme expects, so a 24px panel gets a
# 24px icon rather than scaling the 256px one down to mush.
if [ -d "$SRC/assets/hicolor" ]; then
    (cd "$SRC/assets/hicolor" && find . -name '*.png' -print0) |
    while IFS= read -r -d '' rel; do
        install -Dm0644 "$SRC/assets/hicolor/$rel" "$ICON_ROOT/${rel#./}"
    done
fi

# --------------------------------------------------- desktop entry, absolute
# Exec is the full path: ~/.local/bin is not on PATH for the process that
# launches desktop files on every distribution, and a bare name silently
# fails to start with no error anywhere the user can see.
sed "s|^Exec=evesgarden|Exec=$LAUNCH|" "$SRC/$APP_ID.desktop" > "$DESKTOP"
chmod 0644 "$DESKTOP"

refresh_caches

echo
echo "Installed Eve's Garden."
echo "  launcher : $LAUNCH"
echo "  desktop  : $DESKTOP"
echo "  app menu : search 'Eve's Garden' in your launcher (wofi/rofi/fuzzel)"
echo
echo "Music downloads to ~/Music/Eve's Garden. Nothing to configure --"
echo "searching, downloading and playback all work on first launch."

if ! command -v playerctl >/dev/null 2>&1; then
    echo
    echo "note: 'playerctl' is not installed. The player already publishes"
    echo "      itself over MPRIS, so GNOME's and KDE's panels will find it,"
    echo "      but a bare Hyprland/sway setup binds its media keys through"
    echo "      playerctl -- install it to make those keys work."
fi

case ":$PATH:" in
    *":$BIN:"*) : ;;
    *) echo
       echo "note: $BIN is not on your PATH -- launch from the menu, or add"
       echo "      it to PATH to run 'evesgarden' in a terminal." ;;
esac
