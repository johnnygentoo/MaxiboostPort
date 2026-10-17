#!/usr/bin/env bash
# Adds the GUNDAM VERSUS launcher to the application menu (~/.local/share/applications) for this
# checkout, with its icon in the user's icon theme (hicolor: launcher/gundam.svg and PNGs of it,
# named io.github.gvport.Launcher). The Bloodborne and GODZILLA launchers' entries
# (io.github.bbport.Launcher, io.github.gzport.Launcher) are left alone.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
id=io.github.gvport.Launcher
icons=$data/icons/hicolor
mkdir -p "$icons/scalable/apps"
cp "$here/gundam.svg" "$icons/scalable/apps/$id.svg"
# PNGs for menus and docks that do not draw SVG icons (rsvg-convert, else ImageMagick).
for size in 48 64 128 256 512; do
    mkdir -p "$icons/${size}x${size}/apps"
    out=$icons/${size}x${size}/apps/$id.png
    if command -v rsvg-convert >/dev/null; then
        rsvg-convert -w "$size" -h "$size" "$here/gundam.svg" -o "$out"
    elif command -v magick >/dev/null; then
        magick -background none "$here/gundam.svg" -resize "${size}x${size}" "$out"
    fi
done
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$icons" 2>/dev/null || true
dest=$data/applications
mkdir -p "$dest"
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$id|" -e "s|@PATH@|$(dirname -- "$here")|" \
    "$here/$id.desktop.in" > "$dest/$id.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database -q "$dest" 2>/dev/null || true
echo "Installed $dest/$id.desktop (icon: $icons/scalable/apps/$id.svg)"
