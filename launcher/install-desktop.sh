#!/usr/bin/env bash
# Adds the GODZILLA launcher to the application menu (~/.local/share/applications) for this
# checkout, with its icon in the user's icon theme (hicolor: launcher/godzilla.svg and PNGs of it,
# named io.github.gzport.Launcher). A Bloodborne launcher's entry (io.github.bbport.Launcher) is
# left alone.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
id=io.github.gzport.Launcher
icons=$data/icons/hicolor
mkdir -p "$icons/scalable/apps"
cp "$here/godzilla.svg" "$icons/scalable/apps/$id.svg"
# PNGs for menus and docks that do not draw SVG icons (rsvg-convert, else ImageMagick).
for size in 48 64 128 256 512; do
    mkdir -p "$icons/${size}x${size}/apps"
    out=$icons/${size}x${size}/apps/$id.png
    if command -v rsvg-convert >/dev/null; then
        rsvg-convert -w "$size" -h "$size" "$here/godzilla.svg" -o "$out"
    elif command -v magick >/dev/null; then
        magick -background none "$here/godzilla.svg" -resize "${size}x${size}" "$out"
    fi
done
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$icons" 2>/dev/null || true
dest=$data/applications
mkdir -p "$dest"
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$id|" -e "s|@PATH@|$(dirname -- "$here")|" \
    "$here/$id.desktop.in" > "$dest/$id.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database -q "$dest" 2>/dev/null || true
echo "Installed $dest/$id.desktop (icon: $icons/scalable/apps/$id.svg)"
