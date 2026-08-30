#!/usr/bin/env bash
# Installs betterbanana for the current user: binaries, icon, desktop entry, and a
# systemd user service so the engine is running before you need it.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor/scalable/apps"
UNITS="$HOME/.config/systemd/user"
PWCONF="$HOME/.config/pipewire/pipewire.conf.d"

[ -x "$ROOT/build/bb-engine" ] || { echo "run 'make' first"; exit 1; }
[ -x "$ROOT/build/bb-gui" ]    || { echo "run 'make gui' first"; exit 1; }

mkdir -p "$BIN" "$APPS" "$ICONS" "$UNITS" "$PWCONF"
install -m755 "$ROOT/build/bb-engine" "$ROOT/build/bb-gui" "$ROOT/build/bb-ctl" "$BIN/"
install -m755 "$ROOT/tools/bb-stream-guard" "$BIN/bb-stream-guard"
install -m755 "$ROOT/tools/bb-health" "$BIN/bb-health"
install -m644 "$ROOT/packaging/betterbanana.svg"            "$ICONS/betterbanana.svg"
install -m644 "$ROOT/packaging/betterbanana.desktop"        "$APPS/betterbanana.desktop"
install -m644 "$ROOT/packaging/betterbanana-engine.service" "$UNITS/betterbanana-engine.service"
install -m644 "$ROOT/packaging/betterbanana-stream-guard.service" "$UNITS/betterbanana-stream-guard.service"
install -m644 "$ROOT/packaging/betterbanana-health.service" "$UNITS/betterbanana-health.service"
install -m644 "$ROOT/packaging/99-bb-stream.conf" "$PWCONF/99-bb-stream.conf"

command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" || true
command -v gtk-update-icon-cache >/dev/null && \
  gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

systemctl --user daemon-reload
systemctl --user enable betterbanana-engine.service
# Safe to run unconditionally: with no stream bus configured it only stops
# Discord's screen-share capture from picking up buses carrying the AUX strip,
# which is what makes callers hear themselves echoed inside your stream.
systemctl --user enable betterbanana-stream-guard.service
# Watchdog: the engine can lose every node while still reporting healthy, and
# nothing else in the system notices.
systemctl --user enable betterbanana-health.service
# Restart rather than just start: installing replaces the binary's inode, so a
# service that is already running would keep executing the previous build.
systemctl --user restart betterbanana-engine.service
systemctl --user restart betterbanana-stream-guard.service
systemctl --user restart betterbanana-health.service

echo
echo "Installed to $BIN"
echo "Engine service: systemctl --user status betterbanana-engine"
echo "Stream guard:   systemctl --user status betterbanana-stream-guard"
echo "Watchdog:       systemctl --user status betterbanana-health"
echo "Make sure $BIN is on your PATH."
echo
echo "Sharing your screen on Discord? Set up the stream bus so callers do not"
echo "hear themselves echoed back (see README, 'Discord screen sharing'):"
echo "    bb-ctl route out A3 betterbanana_stream"
