#!/usr/bin/env bash
# Rasterise packaging/betterbanana.svg into the sized PNGs under packaging/icons.
#
# The hicolor theme is searched by size, and a panel that does not link librsvg
# only ever sees the PNG directories -- an icon shipped as scalable/ alone comes
# up as the generic placeholder there. The output is committed so that neither a
# build nor a package needs rsvg-convert; run this after editing the SVG.
set -eu
cd "$(dirname "$0")"

conv=""
for c in rsvg-convert inkscape magick convert; do
    command -v "$c" >/dev/null && { conv="$c"; break; }
done
[ -n "$conv" ] || { echo "need one of rsvg-convert, inkscape or magick" >&2; exit 1; }

for s in 16 24 32 48 64 128 256; do
    out="icons/${s}x${s}/apps/betterbanana.png"
    mkdir -p "$(dirname "$out")"
    case "$conv" in
      rsvg-convert) rsvg-convert -w "$s" -h "$s" betterbanana.svg -o "$out" ;;
      inkscape)     inkscape -w "$s" -h "$s" -o "$out" betterbanana.svg >/dev/null ;;
      # -background none first: magick otherwise flattens onto white.
      *)            "$conv" -background none -density 384 betterbanana.svg \
                      -resize "${s}x${s}" "$out" ;;
    esac
    printf '  %-34s %s\n' "$out" "$(du -h "$out" | cut -f1)"
done
