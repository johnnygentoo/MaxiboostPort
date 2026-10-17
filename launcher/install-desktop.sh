#!/usr/bin/env bash
# Adds the GODZILLA launcher to the application menu (~/.local/share/applications) for this
# checkout. A Bloodborne launcher's entry (io.github.bbport.Launcher) is left alone.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
dest=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$dest"
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$here/godzilla.svg|" "$here/io.github.gzport.Launcher.desktop.in" \
    > "$dest/io.github.gzport.Launcher.desktop"
echo "Installed $dest/io.github.gzport.Launcher.desktop"
