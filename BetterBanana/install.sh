#!/usr/bin/env bash
# BetterBanana one-command installer.
#
#   curl -fsSL https://raw.githubusercontent.com/Zykoraa/Linux-Apps/main/BetterBanana/install.sh | bash
#
# Installs dependencies, builds, installs into ~/.local/bin, and starts the
# audio engine as a systemd user service. Safe to re-run; it updates in place.
set -euo pipefail

REPO_URL="https://github.com/Zykoraa/Linux-Apps.git"
SRC_DIR="${BETTERBANANA_SRC:-$HOME/.local/src/Linux-Apps}"
BIN_DIR="$HOME/.local/bin"

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- prechecks --
[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not root. The engine
       runs as a per-user service and installs into your home directory."

command -v systemctl >/dev/null || warn "systemd not found; the engine will not autostart."
pgrep -x pipewire >/dev/null 2>&1 || warn "PipeWire does not appear to be running."

# ------------------------------------------------------------- dependencies --
missing_pkgs() {
    local need=()
    pkg-config --exists libpipewire-0.3 2>/dev/null || need+=(pipewire)
    pkg-config --exists sndfile         2>/dev/null || need+=(sndfile)
    pkg-config --exists Qt6Widgets      2>/dev/null || need+=(qt6)
    command -v g++  >/dev/null || need+=(compiler)
    command -v git  >/dev/null || need+=(git)
    # moc ships separately from the Qt runtime on most distributions.
    local moc
    moc="$(qmake6 -query QT_HOST_LIBEXECS 2>/dev/null)/moc"
    [ -x "$moc" ] || [ -x /usr/lib/qt6/moc ] || [ -x /usr/lib/qt6/libexec/moc ] \
      || [ -x /usr/lib/x86_64-linux-gnu/qt6/libexec/moc ] || need+=(qt6-tools)
    printf '%s\n' "${need[@]:-}"
}

install_deps() {
    local need
    need="$(missing_pkgs | grep -v '^$' || true)"
    [ -z "$need" ] && { say "All dependencies already present."; return; }

    say "Missing: $(echo "$need" | tr '\n' ' ')"
    local pm cmd
    if   command -v pacman  >/dev/null; then pm=pacman
    elif command -v apt-get >/dev/null; then pm=apt
    elif command -v dnf     >/dev/null; then pm=dnf
    elif command -v zypper  >/dev/null; then pm=zypper
    else
        die "Unrecognised distribution. Install these yourself, then re-run:
       a C++ compiler, git, pkg-config, PipeWire dev headers,
       Qt6 base + tools (moc), libsndfile, and pactl."
    fi

    case "$pm" in
      pacman) cmd="sudo pacman -S --needed --noconfirm base-devel git pkgconf pipewire qt6-base qt6-tools libsndfile libpulse" ;;
      apt)    cmd="sudo apt-get update && sudo apt-get install -y build-essential git pkg-config libpipewire-0.3-dev qt6-base-dev qt6-base-dev-tools libsndfile1-dev pulseaudio-utils" ;;
      dnf)    cmd="sudo dnf install -y gcc-c++ git pkgconf pipewire-devel qt6-qtbase-devel libsndfile-devel pulseaudio-utils" ;;
      zypper) cmd="sudo zypper install -y gcc-c++ git pkg-config pipewire-devel qt6-base-devel libsndfile-devel pulseaudio-utils" ;;
    esac

    say "Installing dependencies with $pm (you will be asked for your password)"
    echo "    $cmd"
    eval "$cmd" || die "Dependency installation failed. Install them manually and re-run."
}

# -------------------------------------------------------------- get sources --
# Works both when piped from curl and when run from inside a checkout.
find_source() {
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
    if [ -n "$here" ] && [ -f "$here/Makefile" ] && [ -d "$here/engine" ]; then
        echo "$here"; return
    fi
    if [ -d "$SRC_DIR/.git" ]; then
        say "Updating existing checkout in $SRC_DIR" >&2
        git -C "$SRC_DIR" pull --ff-only >&2 || warn "Could not fast-forward; using what is there." >&2
    else
        say "Cloning into $SRC_DIR" >&2
        mkdir -p "$(dirname "$SRC_DIR")"
        git clone --depth 1 "$REPO_URL" "$SRC_DIR" >&2
    fi
    echo "$SRC_DIR/BetterBanana"
}

# ---------------------------------------------------------------------- run --
say "BetterBanana installer"
install_deps
SRC="$(find_source)"
[ -f "$SRC/Makefile" ] || die "Could not locate the BetterBanana sources."
say "Building in $SRC"
make -C "$SRC" all gui

say "Installing to $BIN_DIR"
"$SRC/packaging/install.sh"

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) warn "$BIN_DIR is not on your PATH. Add this to your shell rc:"
     echo '       export PATH="$HOME/.local/bin:$PATH"' ;;
esac

cat <<'DONE'

  BetterBanana is installed and the engine is running.

    bb-gui              open the mixer
    bb-ctl status       show routing from the shell
    systemctl --user status betterbanana-engine

  Point an application's output at "BetterBanana VAIO" (or a Cable), and set
  your chat app's microphone to "BetterBanana Out B1".

DONE
