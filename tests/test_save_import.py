from paths import ROOT  # noqa: F401
import os
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

import save_import
from test_game_check import param_sfo


def save_folder(path, marker, files=('DATA.BIN',)):
    path.mkdir(parents=True)
    for name in files:
        (path / name).write_bytes(marker + name.encode())
    return path


class SaveImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.user = self.root / 'user'
        self.game = self.root / 'CUSA08790'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(param_sfo({'TITLE_ID': 'CUSA08790'}))

    def tearDown(self):
        self.tmp.cleanup()

    def test_save_id_from_the_game_or_the_saves_there(self):
        self.assertEqual(save_import.save_id(self.user, self.game), 'CUSA08790')
        (self.game / 'sce_sys/param.sfo').write_bytes(
            param_sfo({'TITLE_ID': 'CUSA08790', 'INSTALL_DIR_SAVEDATA': 'CUSA99999'}))
        self.assertEqual(save_import.save_id(self.user, self.game), 'CUSA99999')
        with self.assertRaises(save_import.SaveImportError):
            save_import.save_id(self.user, None)
        save_folder(self.user / 'savedata/1/CUSA08790/SAVEDATA00', b'x')
        self.assertEqual(save_import.save_id(self.user, None), 'CUSA08790')

    def test_finds_shadps4_and_exported_saves_newest_first(self):
        shad = self.root / 'shadPS4'
        old = save_folder(shad / 'savedata/1/CUSA08790/SAVEDATA00', b'old')
        (old / 'sce_sys').mkdir()
        new = save_folder(self.root / 'usb/PS4/APOLLO/CUSA08790_SAVEDATA00', b'new', ('data.bin',))
        # Another folder with a DATA.BIN, or a SAVEDATA00 without one, is no save folder.
        save_folder(self.root / 'other/SAVE', b'x')
        (self.root / 'empty/SAVEDATA00').mkdir(parents=True)
        for folder, when in ((old, 1000), (new, 2000)):
            for path in save_import.save_files(folder):
                os.utime(path, (when, when))
        found = [folder for folder, _ in save_import.find(self.root)]
        self.assertEqual(found, [new, old])
        self.assertEqual(save_import.find(old), [(old, 1000)])

    def test_encrypted_console_copy(self):
        (self.root / 'PS4/SAVEDATA/abc/CUSA08790').mkdir(parents=True)
        (self.root / 'PS4/SAVEDATA/abc/CUSA08790/sdimg_SAVEDATA00').write_bytes(b'')
        self.assertEqual(save_import.find(self.root), [])
        self.assertTrue(save_import.encrypted_ps4_save(self.root))
        self.assertFalse(save_import.encrypted_ps4_save(self.game))

    def test_import_backs_up_and_replaces_the_save_files_only(self):
        target = save_folder(self.user / 'savedata/1/CUSA08790/SAVEDATA00', b'mine')
        meta = self.user / 'savedata/1/CUSA08790/SAVEDATA00.sce_sys'
        meta.mkdir()
        (meta / 'param.bin').write_bytes(b'param')
        source = save_folder(self.root / 'shadPS4/savedata/1/CUSA08790/SAVEDATA00', b'theirs')
        (source / 'sce_sys').mkdir()
        (source / 'sce_sys/param.sfo').write_bytes(b'sfo')
        result, backup = save_import.import_saves(source, self.user, self.game,
                                                  now=datetime(2026, 10, 17, 12, 0, 0))
        self.assertEqual(result, target)
        self.assertEqual(sorted(p.name for p in target.iterdir()), ['DATA.BIN'])
        self.assertEqual((target / 'DATA.BIN').read_bytes(), b'theirsDATA.BIN')
        self.assertEqual((meta / 'param.bin').read_bytes(), b'param')
        self.assertEqual(backup, self.user / 'savedata-backups/2026-10-17_120000')
        kept = backup / 'CUSA08790/SAVEDATA00'
        self.assertEqual((kept / 'DATA.BIN').read_bytes(), b'mineDATA.BIN')
        self.assertEqual((backup / 'CUSA08790/SAVEDATA00.sce_sys/param.bin').read_bytes(), b'param')
        self.assertFalse(any(p.name.endswith(('.importing', '.old')) for p in target.parent.iterdir()))

    def test_first_import_without_saves(self):
        source = save_folder(self.root / 'export/CUSA08790_SAVEDATA00', b'theirs', ('data.bin',))
        target, backup = save_import.import_saves(source, self.user, self.game)
        self.assertIsNone(backup)
        self.assertEqual(target, self.user / 'savedata/1/CUSA08790/SAVEDATA00')
        self.assertEqual((target / 'DATA.BIN').read_bytes(), b'theirsdata.bin')
        self.assertTrue((target.parent / 'SAVEDATA00.sce_sys').is_dir())

    def test_refusals(self):
        with self.assertRaises(save_import.SaveImportError):
            save_import.import_saves(self.root, self.user, self.game)
        target = save_folder(self.user / 'savedata/1/CUSA08790/SAVEDATA00', b'mine')
        with self.assertRaises(save_import.SaveImportError):
            save_import.import_saves(target, self.user, self.game)

    def test_failed_copy_keeps_the_saves(self):
        target = save_folder(self.user / 'savedata/1/CUSA08790/SAVEDATA00', b'mine')
        source = save_folder(self.root / 'export/SAVEDATA00', b'theirs')
        with mock.patch.object(save_import.shutil, 'copy2', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                save_import.import_saves(source, self.user, self.game)
        self.assertEqual((target / 'DATA.BIN').read_bytes(), b'mineDATA.BIN')
        self.assertFalse((target.parent / 'SAVEDATA00.importing').exists())


if __name__ == '__main__':
    unittest.main()
