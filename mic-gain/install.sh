#!/bin/sh
# Install mic-gain into ~/.local/bin. No root, no build step.
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/mic-gain
dest="${XDG_BIN_HOME:-$HOME/.local/bin}"

command -v python3 >/dev/null 2>&1 || {
    echo "mic-gain needs python3." >&2
    exit 1
}
command -v pw-record >/dev/null 2>&1 || {
    echo "Warning: pw-record not found. mic-gain needs PipeWire to capture audio." >&2
}

mkdir -p "$dest"
install -m 755 "$src" "$dest/mic-gain"
echo "Installed $dest/mic-gain"

case ":$PATH:" in
    *":$dest:"*) echo "Run: mic-gain" ;;
    *) echo "Note: $dest is not on your PATH. Add it, or run $dest/mic-gain" ;;
esac
