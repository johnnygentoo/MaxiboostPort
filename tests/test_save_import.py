from paths import ROOT  # noqa: F401
import os
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

import save_import
from test_game_check import param_sfo


def save_folder(path, marker, files=('memory.dat',)):
    path.mkdir(parents=True)
    for name in files:
        (path / name).write_bytes(marker + name.encode())
    return path


def touch(folder, when):
    for path in save_import.save_files(folder):
        os.utime(path, (when, when))


class SaveImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.user = self.root / 'user'
        self.saves = self.user / 'savedata/1'
        self.game = self.root / 'CUSA18758'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(param_sfo({'TITLE_ID': 'CUSA18758'}))

    def tearDown(self):
        self.tmp.cleanup()

    def test_save_id_from_the_game_or_the_saves_there(self):
        self.assertEqual(save_import.save_id(self.user, self.game), 'CUSA18758')
        (self.game / 'sce_sys/param.sfo').write_bytes(
            param_sfo({'TITLE_ID': 'CUSA18758', 'INSTALL_DIR_SAVEDATA': 'CUSA99999'}))
        self.assertEqual(save_import.save_id(self.user, self.game), 'CUSA99999')
        with self.assertRaises(save_import.SaveImportError):
            save_import.save_id(self.user, None)
        save_folder(self.saves / 'CUSA18758.memory', b'x')
        self.assertEqual(save_import.save_id(self.user, None), 'CUSA18758')

    def test_save_id_from_replays_alone(self):
        save_folder(self.saves / 'CUSA18758/REPLAY', b'x', ('REPLAY00.DAT',))
        self.assertEqual(save_import.save_id(self.user, None), 'CUSA18758')

    def test_kinds(self):
        port = save_folder(self.root / 'a/CUSA18758.memory', b'x', ('memory.dat', 'param.bin'))
        shad = save_folder(self.root / 'b/sce_sdmemory', b'x', ('MEMORY.DAT',))
        replays = save_folder(self.root / 'c/CUSA18758_REPLAY', b'x', ('REPLAY00.DAT',))
        self.assertEqual(save_import.save_kind(port), save_import.PROGRESS)
        self.assertEqual(save_import.save_kind(shad), save_import.PROGRESS)
        self.assertEqual(save_import.save_kind(replays), save_import.REPLAYS)
        # Our param.bin alone is no save; neither is an empty REPLAY or a folder of other files.
        self.assertIsNone(save_import.save_kind(save_folder(self.root / 'd/X.memory', b'x', ('param.bin',))))
        (self.root / 'e/REPLAY').mkdir(parents=True)
        self.assertIsNone(save_import.save_kind(self.root / 'e/REPLAY'))
        self.assertIsNone(save_import.save_kind(save_folder(self.root / 'f/SAVE', b'x', ('DATA.BIN',))))

    def test_finds_progress_and_replays_newest_first(self):
        shad = self.root / 'shadPS4/savedata/1/CUSA18758'
        old = save_folder(shad / 'sce_sdmemory', b'old')
        (old / 'sce_sys').mkdir()
        replays = save_folder(shad / 'REPLAY', b'r', ('REPLAY00.DAT',))
        new = save_folder(self.root / 'usb/PS4/APOLLO/CUSA18758_sce_sdmemory', b'new')
        (self.root / 'empty/REPLAY').mkdir(parents=True)
        for folder, when in ((old, 1000), (replays, 1500), (new, 2000)):
            touch(folder, when)
        found = save_import.find(self.root)
        self.assertEqual([folder for folder, _ in found], [new, replays, old])
        self.assertEqual(save_import.newest(found), [(new, 2000), (replays, 1500)])
        self.assertEqual(save_import.find(old), [(old, 1000)])

    def test_encrypted_console_copy(self):
        (self.root / 'PS4/SAVEDATA/abc/CUSA18758').mkdir(parents=True)
        (self.root / 'PS4/SAVEDATA/abc/CUSA18758/sdimg_sce_sdmemory').write_bytes(b'')
        self.assertEqual(save_import.find(self.root), [])
        self.assertTrue(save_import.encrypted_ps4_save(self.root))
        self.assertFalse(save_import.encrypted_ps4_save(self.game))

    def test_progress_import_backs_up_and_keeps_our_param(self):
        folder = save_folder(self.saves / 'CUSA18758.memory', b'mine', ('memory.dat', 'param.bin'))
        source = save_folder(self.root / 'shadPS4/savedata/1/CUSA18758/sce_sdmemory', b'theirs',
                             ('MEMORY.DAT',))
        (source / 'sce_sys').mkdir()
        (source / 'sce_sys/param.sfo').write_bytes(b'sfo')
        result, backup = save_import.import_saves(source, self.user, self.game,
                                                  now=datetime(2026, 10, 19, 12, 0, 0))
        self.assertEqual(result, folder)
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ['memory.dat', 'param.bin'])
        self.assertEqual((folder / 'memory.dat').read_bytes(), b'theirsMEMORY.DAT')
        self.assertEqual((folder / 'param.bin').read_bytes(), b'mineparam.bin')
        self.assertEqual(backup, self.user / 'savedata-backups/2026-10-19_120000')
        kept = backup / 'CUSA18758.memory'
        self.assertEqual((kept / 'memory.dat').read_bytes(), b'minememory.dat')
        self.assertEqual((kept / 'param.bin').read_bytes(), b'mineparam.bin')

    def test_first_progress_import(self):
        source = save_folder(self.root / 'export/CUSA18758_sce_sdmemory', b'theirs')
        target, backup = save_import.import_saves(source, self.user, self.game)
        self.assertIsNone(backup)
        self.assertEqual(target, self.saves / 'CUSA18758.memory')
        self.assertEqual((target / 'memory.dat').read_bytes(), b'theirsmemory.dat')

    def test_replays_import_backs_up_and_replaces_the_files_only(self):
        target = save_folder(self.saves / 'CUSA18758/REPLAY', b'mine', ('REPLAY00.DAT',))
        meta = self.saves / 'CUSA18758/REPLAY.sce_sys'
        meta.mkdir()
        (meta / 'param.bin').write_bytes(b'param')
        source = save_folder(self.root / 'shadPS4/savedata/1/CUSA18758/REPLAY', b'theirs',
                             ('REPLAY01.DAT',))
        (source / 'sce_sys').mkdir()
        result, backup = save_import.import_saves(source, self.user, self.game,
                                                  now=datetime(2026, 10, 19, 12, 0, 0))
        self.assertEqual(result, target)
        self.assertEqual(sorted(p.name for p in target.iterdir()), ['REPLAY01.DAT'])
        self.assertEqual((meta / 'param.bin').read_bytes(), b'param')
        kept = backup / 'CUSA18758'
        self.assertEqual((kept / 'REPLAY/REPLAY00.DAT').read_bytes(), b'mineREPLAY00.DAT')
        self.assertEqual((kept / 'REPLAY.sce_sys/param.bin').read_bytes(), b'param')
        self.assertFalse(any(p.name.endswith(('.importing', '.old')) for p in target.parent.iterdir()))

    def test_first_replays_import(self):
        source = save_folder(self.root / 'export/CUSA18758_REPLAY', b'theirs', ('REPLAY00.DAT',))
        target, backup = save_import.import_saves(source, self.user, self.game)
        self.assertIsNone(backup)
        self.assertEqual(target, self.saves / 'CUSA18758/REPLAY')
        self.assertTrue((target.parent / 'REPLAY.sce_sys').is_dir())

    def test_refusals(self):
        with self.assertRaises(save_import.SaveImportError):
            save_import.import_saves(self.root, self.user, self.game)
        mine = save_folder(self.saves / 'CUSA18758.memory', b'mine')
        with self.assertRaises(save_import.SaveImportError):
            save_import.import_saves(mine, self.user, self.game)
        replays = save_folder(self.saves / 'CUSA18758/REPLAY', b'mine', ('REPLAY00.DAT',))
        with self.assertRaises(save_import.SaveImportError):
            save_import.import_saves(replays, self.user, self.game)

    def test_failed_copy_keeps_the_saves(self):
        folder = save_folder(self.saves / 'CUSA18758.memory', b'mine')
        target = save_folder(self.saves / 'CUSA18758/REPLAY', b'mine', ('REPLAY00.DAT',))
        progress = save_folder(self.root / 'export/sce_sdmemory', b'theirs')
        replays = save_folder(self.root / 'export/REPLAY', b'theirs', ('REPLAY01.DAT',))
        with mock.patch.object(save_import.shutil, 'copy2', side_effect=OSError('disk full')):
            for source in (progress, replays):
                with self.assertRaises(OSError):
                    save_import.import_saves(source, self.user, self.game)
        self.assertEqual((folder / 'memory.dat').read_bytes(), b'minememory.dat')
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ['memory.dat'])
        self.assertEqual((target / 'REPLAY00.DAT').read_bytes(), b'mineREPLAY00.DAT')
        self.assertFalse((target.parent / 'REPLAY.importing').exists())


if __name__ == '__main__':
    unittest.main()
