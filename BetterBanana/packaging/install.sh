#!/usr/bin/env bash
# Installs betterbanana for the current user: binaries, icon, desktop entry, and a
# systemd user service so the engine is running before you need it.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor/scalable/apps"
UNITS="$HOME/.config/systemd/user"

[ -x "$ROOT/build/bb-engine" ] || { echo "run 'make' first"; exit 1; }
[ -x "$ROOT/build/bb-gui" ]    || { echo "run 'make gui' first"; exit 1; }

mkdir -p "$BIN" "$APPS" "$ICONS" "$UNITS"
install -m755 "$ROOT/build/bb-engine" "$ROOT/build/bb-gui" "$ROOT/build/bb-ctl" "$BIN/"
install -m644 "$ROOT/packaging/betterbanana.svg"            "$ICONS/betterbanana.svg"
install -m644 "$ROOT/packaging/betterbanana.desktop"        "$APPS/betterbanana.desktop"
install -m644 "$ROOT/packaging/betterbanana-engine.service" "$UNITS/betterbanana-engine.service"

command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" || true
command -v gtk-update-icon-cache >/dev/null && \
  gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

systemctl --user daemon-reload
systemctl --user enable betterbanana-engine.service
# Restart rather than just start: installing replaces the binary's inode, so a
# service that is already running would keep executing the previous build.
systemctl --user restart betterbanana-engine.service

echo
echo "Installed to $BIN"
echo "Engine service: systemctl --user status betterbanana-engine"
echo "Make sure $BIN is on your PATH."
