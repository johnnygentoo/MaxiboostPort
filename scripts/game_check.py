#!/usr/bin/env python3
"""The game files gzport runs: GODZILLA (PS4, 2015) with the 1.01 update merged in.

Hooks and patches use the addresses of one executable, so the loaded image decides: its hash is
the same whatever tool dumped it (the SELF headers around it differ). The base game 1.00 is
recognised and asked for its update. BB_SKIP_GAME_CHECK=1 skips the check.

Run as a script, it checks that a game folder has the files the game opens first:
game_check.py GAME_DIR
"""
import hashlib
import os
import sys
from pathlib import Path

# The retail releases: the North American one (CUSA02046) is the one checked; the executable
# decides in the end (SUPPORTED_IMAGE), so another region with the same 1.01 eboot runs too.
SUPPORTED_TITLES = ('CUSA02046',)
SUPPORTED_VERSION = '01.01'
SUPPORTED_IMAGE = 'aa191628d842649f553ffffcff88bd5c7d977d12a45aed7b2b9743cd2f81f77c'
BASE_IMAGE = '1284618c6d4d14ddbe31800573a2daf6cbe2e5a4519c11b15f063350127e87b7'  # 1.00
# The game opens these first; a folder without them is not a whole extraction.
REQUIRED_FILES = ('system.pac', 'game_en.pac', 'sound')


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span  # same directory
    elf, header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def missing_files(game):
    root = Path(game)
    return [name for name in REQUIRED_FILES if not (root / name).exists()]


def problem(game, image_hash=None):
    """None for the supported game, else (kind, title, version): kind is 'missing_update' (the
    base game), 'wrong_eboot' (param.sfo says 1.01, eboot.bin is another version), 'other_title',
    'unreadable' or 'damaged_files' (the right executable, game files missing)."""
    if os.environ.get('BB_SKIP_GAME_CHECK') == '1':
        return None
    from prepare import sfo
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    try:
        image_hash = image_hash or image_sha256(game)
        if image_hash == SUPPORTED_IMAGE:
            return ('damaged_files', title, version) if missing_files(game) else None
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return 'unreadable', title, version
    if image_hash == BASE_IMAGE:
        return 'missing_update', title, version
    if title not in SUPPORTED_TITLES:
        return 'other_title', title, version
    if version != SUPPORTED_VERSION:
        return 'missing_update', title, version
    return 'wrong_eboot', title, version


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f'Found {title} version {version} (sce_sys/param.sfo).'
    return {
        'missing_update': f'{found} gzport needs the 1.01 update merged into the game folder: copy '
                          'everything from the dumped update (e.g. CUSA02046-patch) into the game '
                          'folder, replacing files (eboot.bin and sce_sys too).',
        'wrong_eboot': f'{found} param.sfo is from 1.01 but eboot.bin is not: copy eboot.bin from '
                       'the dumped 1.01 update into the game folder, replacing the old one.',
        'other_title': f'{found} This is not a retail release of GODZILLA that gzport knows: it '
                       'runs GODZILLA (CUSA02046) with update 1.01.',
        'unreadable': f'{found} eboot.bin could not be read as a decrypted PS4 executable: dump '
                      'the game and the 1.01 update again.',
        'damaged_files': f'{found} The game folder is incomplete: one of '
                         f'{", ".join(REQUIRED_FILES)} is not there. Extract the game again.',
    }[kind]


def main(argv):
    """game_check.py GAME_DIR: the result of the check, in words."""
    if len(argv) != 2:
        print('Usage: game_check.py GAME_DIR', file=sys.stderr)
        return 2
    found = problem(argv[1])
    print(explain(*found) if found else 'GODZILLA 1.01: supported')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
