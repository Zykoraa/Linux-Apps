#!/usr/bin/env bash
# Build Evecord and install it for the current user (no root needed).
#
#   ./install.sh               build and install (re-run to update)
#   ./install.sh --uninstall   remove the installed app; ~/.config/evecord is kept
#
# Installs:
#   ~/.local/lib/evecord/                    Electron runtime, binary renamed to evecord
#   ~/.local/lib/evecord/resources/app/      Evecord itself
#   ~/.local/bin/evecord                     symlink to the binary
#   ~/.local/share/applications/evecord.desktop
#   ~/.local/share/icons/hicolor/*/apps/evecord.*
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

PREFIX="${PREFIX:-$HOME/.local}"
LIB="$PREFIX/lib/evecord"
BIN="$PREFIX/bin/evecord"
APPS="$PREFIX/share/applications"
ICONS="$PREFIX/share/icons/hicolor"
DESKTOP="$APPS/evecord.desktop"

say() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -rf "$LIB"
    rm -f "$BIN" "$DESKTOP" "$ICONS/512x512/apps/evecord.png" "$ICONS/scalable/apps/evecord.svg"
    command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
    say "Evecord removed. Settings and login are still in ${XDG_CONFIG_HOME:-$HOME/.config}/evecord."
    exit 0
fi

command -v node >/dev/null || die "node is required (22 or newer)"
node -e 'process.exit(parseInt(process.versions.node) < 22 ? 1 : 0)' || die "node 22 or newer is required"
command -v pnpm >/dev/null || die "pnpm is required (npm i -g pnpm)"

say "Installing dependencies"
pnpm install --frozen-lockfile

# pnpm does not run electron's own download step, so fetch the runtime explicitly.
if [[ ! -x node_modules/electron/dist/electron ]]; then
    say "Downloading Electron"
    node node_modules/electron/install.js
fi

say "Building"
pnpm run build

say "Assembling $LIB"
staging="$LIB.new"
rm -rf "$staging"
mkdir -p "$(dirname "$LIB")"
cp -a node_modules/electron/dist "$staging"
mv "$staging/electron" "$staging/evecord"
# Without default_app.asar Electron runs resources/app instead of its demo app.
rm -f "$staging/resources/default_app.asar"
mkdir -p "$staging/resources/app"
cp -a dist static package.json LICENSE "$staging/resources/app/"

# Swap in the new copy. A running Evecord keeps the files it already has open;
# restart it to pick up the update.
if [[ -d "$LIB" ]]; then
    rm -rf "$LIB.old"
    mv "$LIB" "$LIB.old"
fi
mv "$staging" "$LIB"
rm -rf "$LIB.old"

mkdir -p "$(dirname "$BIN")" "$APPS" "$ICONS/512x512/apps" "$ICONS/scalable/apps"
ln -sfn "$LIB/evecord" "$BIN"
install -m644 static/icon.png "$ICONS/512x512/apps/evecord.png"
install -m644 static/icon.svg "$ICONS/scalable/apps/evecord.svg"

# Exec must be absolute: the uwsm session replaces PATH, so a bare command name
# launches nothing from an app menu.
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Evecord
GenericName=Discord Client
Comment=Discord with Vencord, made for Linux
Exec="$LIB/evecord" %U
Icon=evecord
Terminal=false
Categories=Network;InstantMessaging;Chat;
Keywords=discord;vencord;chat;voice;
MimeType=x-scheme-handler/discord;
StartupWMClass=evecord
StartupNotify=true
Actions=Minimized;

[Desktop Action Minimized]
Name=Start in tray
Exec="$LIB/evecord" --start-minimized
EOF

command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$ICONS" 2>/dev/null || true

say "Installed. Launch Evecord from your app menu, or run: $BIN"
if pgrep -x evecord >/dev/null; then
    say "Evecord is running: restart it (tray menu, Restart Evecord) to use this build."
fi
