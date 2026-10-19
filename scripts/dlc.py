"""A title's add-ons (DLC) in a folder, and their entitlements in the user directory.

The runtime reports <user>/addcont/<title id>/<entitlement label> as installed (shadPS4's layout,
src/runtime_content.c). A DLC folder may hold:
- dumped add-ons, one folder each, named by the label, with sce_sys/param.sfo (Bloodborne's The
  Old Hunters: its data ships with a patch, the folder is only the entitlement);
- the add-ons' .pkg files as they are. GUNDAM VERSUS' 28 add-ons (mobile suits, navigators) are
  128 KB unlock keys: their data is in the game and its update, the package carries none.

    python3 scripts/dlc.py link --dlc DIR --game GAME_DIR --user USER_DIR
    python3 scripts/dlc.py list --dlc DIR --title CUSA08790
"""
import argparse
import os
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import sfo  # noqa: E402

# A folder of entitlements this script made: <user>/addcont/<title id> holding this file is
# rebuilt every launch; a real folder without it (someone's own) is left alone.
MARKER = '.gvport-dlc'


def pkg_param(path):
    """The param.sfo of a PS4 package (entry 0x1000, not encrypted), or None."""
    try:
        with open(path, 'rb') as f:
            head = f.read(0x20)
            if head[:4] != b'\x7fCNT':
                return None
            count, table = struct.unpack('>I', head[0x10:0x14])[0], struct.unpack('>I', head[0x18:0x1c])[0]
            f.seek(table)
            entries = f.read(count * 32)
            for i in range(count):
                entry_id, _, _, _, offset, size = struct.unpack('>IIIIII', entries[i * 32:i * 32 + 24])
                if entry_id == 0x1000:
                    f.seek(offset)
                    return sfo(f.read(size))
    except (OSError, ValueError, struct.error):
        pass
    return None


def addons(dlc, title_id):
    """{label: source}: this title's add-ons in the folder; source is the add-on's folder, or None
    for a package (only its entitlement is reported)."""
    found = {}
    folder = Path(dlc)
    if not title_id or not folder.is_dir():
        return found
    for entry in sorted(folder.iterdir()):
        if entry.is_dir():
            try:
                info = sfo((entry / 'sce_sys/param.sfo').read_bytes())
            except (OSError, ValueError, IndexError):
                continue
            label = entry.name
        elif entry.suffix.lower() == '.pkg':
            info = pkg_param(entry)
            if not info:
                continue
            label = None
        else:
            continue
        content = str(info.get('CONTENT_ID', ''))
        if info.get('CATEGORY') != 'ac' or content[7:16] != title_id or len(content) != 36:
            continue
        if label is not None and content[20:] != label:
            continue
        found.setdefault(content[20:], entry if label is not None else None)
    return found


def link(dlc, game, user):
    """Makes <user>/addcont/<title id> report the folder's add-ons; returns how many."""
    title = str(sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID', ''))
    found = addons(dlc, title)
    target = Path(user) / 'addcont' / title
    if not found or len(title) != 9:
        return 0
    if target.is_symlink():
        target.unlink()  # the link to a folder of dumped add-ons made by earlier versions
    elif target.is_dir() and not (target / MARKER).exists():
        print(f'DLC: {target} is a folder of your own; left as it is')
        return 0
    elif target.is_dir():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    (target / MARKER).write_text(f'made from {dlc} at each launch (scripts/dlc.py)\n')
    for label, source in found.items():
        if source is None:
            (target / label).mkdir()
        else:
            os.symlink(source.resolve(), target / label)
    return len(found)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('link', 'list'))
    parser.add_argument('--dlc', required=True)
    parser.add_argument('--game')
    parser.add_argument('--user')
    parser.add_argument('--title')
    a = parser.parse_args()
    if a.command == 'list':
        for label, source in addons(a.dlc, a.title).items():
            print(label, source or '(package: entitlement only)')
        return
    count = link(a.dlc, a.game, a.user)
    if count:
        print(f'DLC: {count} add-ons of this game reported installed ({a.dlc})')


if __name__ == '__main__':
    main()
