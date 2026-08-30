#!/usr/bin/env bash
# Glory Injector — user-level installer (no root required).
#
#   ./install.sh              install or upgrade
#   ./install.sh --uninstall  remove
#
# Installs to ~/.local so it appears in wofi/rofi/fuzzel and any app menu.
set -euo pipefail

APP_ID="glory-injector"
SRC="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

SHARE="$HOME/.local/share"
BIN="$HOME/.local/bin"
APP_DIR="$SHARE/$APP_ID"
DESKTOP="$SHARE/applications/$APP_ID.desktop"
ICON_DIR="$SHARE/icons/hicolor/scalable/apps"
ICON="$ICON_DIR/$APP_ID.svg"
LAUNCH="$BIN/$APP_ID"

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$SHARE/applications" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$SHARE/icons/hicolor" >/dev/null 2>&1 || true
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$APP_DIR"
    rm -f "$DESKTOP" "$ICON" "$LAUNCH"
    refresh_caches
    echo "Removed Glory Injector."
    exit 0
fi

# --- sanity check ---
if [ ! -f "$SRC/glory_injector_linux.py" ]; then
    echo "error: glory_injector_linux.py must sit next to install.sh" >&2
    exit 1
fi

# --- optional dependency hint ---
for dep in gdb hyprctl; do
    command -v "$dep" >/dev/null 2>&1 || \
        echo "note: '$dep' not found — install it for full functionality (sudo pacman -S $dep)"
done

# --- install files ---
mkdir -p "$APP_DIR" "$BIN" "$(dirname "$DESKTOP")" "$ICON_DIR"
install -m 0644 "$SRC/glory_injector_linux.py" "$APP_DIR/"
[ -f "$SRC/glory-injector.svg" ] && install -m 0644 "$SRC/glory-injector.svg" "$ICON"
[ -f "$SRC/netanyahu.jpg" ]      && install -m 0644 "$SRC/netanyahu.jpg" "$APP_DIR/"
install -m 0755 "$SRC/glory-injector" "$LAUNCH"

# --- desktop entry with absolute paths (works regardless of PATH) ---
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Glory Injector
GenericName=Shared-object injector
Comment=Inject .so libraries into running processes (gdb/dlopen)
Exec=$LAUNCH
Icon=$ICON
Terminal=false
Categories=Development;Utility;
Keywords=inject;so;dll;dlopen;ptrace;gdb;hyprland;
StartupNotify=true
StartupWMClass=GloryInjector
EOF
chmod 0644 "$DESKTOP"

refresh_caches

echo "Installed Glory Injector."
echo "  launcher : $LAUNCH"
echo "  desktop  : $DESKTOP"
echo "  app menu : search 'Glory Injector' in your launcher (wofi/rofi/fuzzel)"
case ":$PATH:" in
    *":$BIN:"*) : ;;
    *) echo "  note: $BIN is not on your PATH — launch from the menu, or add it to PATH to run 'glory-injector' in a terminal." ;;
esac
