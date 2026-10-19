#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Import MAXIBOOST ON saves from another build: shadPS4, another copy of mbport, a PS4 save
exported decrypted (Apollo Save Tool, Save Wizard), a copy on a USB stick.

    save_import.py --user USER_DIR [--game GAME_DIR] SOURCE

MAXIBOOST ON keeps its progress in save data memory, one 2 MiB blob the game reads and writes
itself, and its replays in a save folder named REPLAY. Under the port (src/runtime_savedata.c)
they are user 1's
    savedata/1/<save id>.memory/memory.dat   (param.bin beside it: the save list's details)
    savedata/1/<save id>/REPLAY/             (REPLAY.sce_sys beside it)
with the save id from param.sfo (INSTALL_DIR_SAVEDATA, else TITLE_ID: CUSA18758; the same rule as
src/probe.c). A progress save is any folder holding memory.dat (the port's <save id>.memory;
shadPS4's and a PS4 export's sce_sdmemory are expected to look the same, not checked yet); a
replays folder is one named REPLAY (or an export named after it, CUSA18758_REPLAY) holding files.
The files need no conversion: the newest progress save and the newest replays folder found are
copied in. What is there is first copied to USER_DIR/savedata-backups/<date>/, and the new files
are staged next to it and swapped in, so a failed copy leaves the saves as they were. A console's
own USB copy (sdimg_*) is encrypted with that console's keys and cannot be read: it has to be
exported decrypted on the PS4.

Idea and rules from Awaken's BloodBorneButPc (SaveImport.cs).
"""
import argparse
from datetime import datetime
from pathlib import Path
import os
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import sfo  # noqa: E402

# Progress: the save data memory file, in <save id>.memory.
MEMORY_FILE = 'memory.dat'
MEMORY_SUFFIX = '.memory'
# Replays: the save folder the game mounts by this name.
SAVE_DIR = 'REPLAY'
PROGRESS, REPLAYS = 'progress', 'replays'
# Folders that hold no saves: game files and our own backups.
SKIPPED = {'sce_sys', 'sce_module', 'sound', 'savedata-backups', 'shader_cache', 'mods', 'trophies'}


class SaveImportError(Exception):
    pass


def save_files(folder):
    """The files of a save folder (what the game wrote), sorted; its subfolders (a shadPS4
    folder's sce_sys) and our param.bin are not the game's."""
    try:
        return sorted(p for p in Path(folder).iterdir()
                      if p.is_file() and not p.name.startswith('.') and p.name != 'param.bin')
    except OSError:
        return []


def save_kind(folder):
    """PROGRESS for a folder holding memory.dat, REPLAYS for a REPLAY folder (or an export named
    after it, CUSA18758_REPLAY) holding files, else None."""
    folder = Path(folder)
    files = save_files(folder)
    if any(p.name.lower() == MEMORY_FILE for p in files):
        return PROGRESS
    if folder.name.upper().endswith(SAVE_DIR) and files:
        return REPLAYS
    return None


def is_save_folder(folder):
    return save_kind(folder) is not None


def find(root, depth=7):
    """Save folders under root (root itself included), newest first: [(folder, newest mtime)]."""
    found = []

    def walk(folder, left):
        if is_save_folder(folder):
            found.append((folder, max(p.stat().st_mtime for p in save_files(folder))))
            return
        if left == 0:
            return
        try:
            children = sorted(p for p in folder.iterdir() if p.is_dir() and not p.is_symlink())
        except OSError:
            return
        for child in children:
            if child.name not in SKIPPED and not child.name.endswith('.sce_sys'):
                walk(child, left - 1)

    walk(Path(root), depth)
    return sorted(found, key=lambda item: item[1], reverse=True)


def newest(found):
    """The newest folder of each kind in find()'s list (progress first): what an import takes."""
    picked = {}
    for folder, when in found:
        picked.setdefault(save_kind(folder), (folder, when))
    return [picked[kind] for kind in (PROGRESS, REPLAYS) if kind in picked]


def encrypted_ps4_save(root, depth=7):
    """A console's USB copy (sdimg_*) under root: encrypted, no PC tool opens it."""
    def walk(folder, left):
        try:
            entries = list(folder.iterdir())
        except OSError:
            return False
        if any(p.is_file() and p.name.startswith('sdimg_') for p in entries):
            return True
        return left > 0 and any(walk(p, left - 1) for p in entries
                                if p.is_dir() and not p.is_symlink() and p.name not in SKIPPED)
    return walk(Path(root), depth)


def save_id(user_dir, game_dir=None):
    """The name the runtime keeps this game's saves under (src/probe.c): param.sfo's
    INSTALL_DIR_SAVEDATA, else TITLE_ID; without a readable game, the one saves already use."""
    if game_dir:
        try:
            info = sfo((Path(game_dir) / 'sce_sys/param.sfo').read_bytes())
        except (OSError, ValueError, IndexError):
            info = {}
        for key in ('INSTALL_DIR_SAVEDATA', 'TITLE_ID'):
            value = str(info.get(key, '')).strip('\0').strip()
            if value:
                return value
    user = Path(user_dir) / 'savedata/1'
    if user.is_dir():
        for entry in sorted(user.iterdir()):
            if entry.name.endswith(MEMORY_SUFFIX) and (entry / MEMORY_FILE).is_file():
                return entry.name[:-len(MEMORY_SUFFIX)]
            if (entry / SAVE_DIR).is_dir():
                return entry.name
    raise SaveImportError('the game folder is not set and there are no saves yet to tell the game id')


def game_running():
    """bb-probe (the game) is running: its saves must not change under it."""
    for comm in Path('/proc').glob('[0-9]*/comm'):
        try:
            if comm.read_text().strip() == 'bb-probe':
                return True
        except OSError:
            continue
    return False


def _backup(user_dir, folder, now):
    """Copies folder (a title's save folder or its .memory one) to savedata-backups/<date>/."""
    stamp = (now or datetime.now()).strftime('%Y-%m-%d_%H%M%S')
    backup = user_dir / 'savedata-backups' / stamp
    shutil.copytree(folder, backup / folder.name, symlinks=True)
    return backup


def _import_progress(source, user_dir, sid, now):
    """memory.dat in as <save id>.memory/memory.dat; our param.bin there is kept."""
    folder = user_dir / 'savedata/1' / (sid + MEMORY_SUFFIX)
    target = folder / MEMORY_FILE
    if source.resolve() == folder.resolve():
        raise SaveImportError('those are already the saves in use')
    backup = _backup(user_dir, folder, now) if target.is_file() else None
    folder.mkdir(parents=True, exist_ok=True)
    memory = next(p for p in save_files(source) if p.name.lower() == MEMORY_FILE)
    staging = folder / (MEMORY_FILE + '.importing')
    try:
        shutil.copy2(memory, staging)
        os.replace(staging, target)
    except OSError:
        staging.unlink(missing_ok=True)
        raise
    return folder, backup


def _import_replays(source, user_dir, sid, now):
    """The replay files in as <save id>/REPLAY; our REPLAY.sce_sys is kept."""
    target = user_dir / 'savedata/1' / sid / SAVE_DIR
    if source.resolve() == target.resolve():
        raise SaveImportError('those are already the saves in use')
    backup = _backup(user_dir, target.parent, now) if save_files(target) else None
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = target.with_name(SAVE_DIR + '.importing')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir()
    try:
        # Only the files: a shadPS4 folder's sce_sys inside is its own bookkeeping.
        for path in save_files(source):
            shutil.copy2(path, staging / path.name)
        old = target.with_name(SAVE_DIR + '.old')
        shutil.rmtree(old, ignore_errors=True)
        if target.exists():
            target.rename(old)
        staging.rename(target)
        shutil.rmtree(old, ignore_errors=True)
    except OSError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    # The runtime writes the save list's details (param.bin) there when the game saves.
    target.with_name(SAVE_DIR + '.sce_sys').mkdir(exist_ok=True)
    return target, backup


def import_saves(source, user_dir, game_dir=None, now=None):
    """Copies a progress save or a replays folder in as user 1's; returns (target folder, backup
    folder or None)."""
    source = Path(source).expanduser()
    kind = save_kind(source)
    if kind is None:
        raise SaveImportError(f'{source} has no MAXIBOOST ON save ({MEMORY_FILE}, or a {SAVE_DIR} '
                              'folder with replays)')
    user_dir = Path(user_dir).expanduser()
    sid = save_id(user_dir, game_dir)
    if kind == PROGRESS:
        return _import_progress(source, user_dir, sid, now)
    return _import_replays(source, user_dir, sid, now)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--user', required=True, help='the user folder (saves, shader cache)')
    parser.add_argument('--game', help='the game folder (its param.sfo gives the save id)')
    parser.add_argument('source', help='a save folder, or a folder to search for them')
    a = parser.parse_args()
    if game_running():
        print('Import failed: close the game first', file=sys.stderr)
        return 1
    found = newest(find(a.source))
    if not found:
        reason = ('it is a PS4 USB copy (sdimg_*), encrypted with the console\'s keys: export it '
                  'decrypted on the PS4 (Apollo Save Tool) or with Save Wizard'
                  if encrypted_ps4_save(a.source)
                  else f'no MAXIBOOST ON saves ({MEMORY_FILE}, {SAVE_DIR})')
        print(f'Import failed: {reason}', file=sys.stderr)
        return 1
    now = datetime.now()
    for source, _when in found:
        try:
            target, backup = import_saves(source, a.user, a.game, now=now)
        except (SaveImportError, OSError) as error:
            print(f'Import failed: {error}', file=sys.stderr)
            return 1
        print(f'Imported {source} into {target}')
        if backup:
            print(f'Previous saves backed up in {backup}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
