from paths import ROOT
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'scripts'))
import dlc  # noqa: E402
from test_game_check import param_sfo  # noqa: E402


def unlock_key(path, content_id, category='ac'):
    """A PS4 package as MAXIBOOST ON's add-ons are: the header, an entry table and its param.sfo
    (entry 0x1000), nothing else."""
    sfo = param_sfo({'CATEGORY': category, 'CONTENT_ID': content_id, 'TITLE_ID': content_id[7:16]})
    header = bytearray(0x100)
    header[:4] = b'\x7fCNT'
    struct.pack_into('>I', header, 0x10, 1)
    struct.pack_into('>I', header, 0x18, 0x80)
    struct.pack_into('>IIIIII', header, 0x80, 0x1000, 0, 0, 0, 0x100, len(sfo))
    path.write_bytes(bytes(header) + sfo)


class DlcTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.dlc = self.root / 'dlc'
        self.dlc.mkdir()
        self.game = self.root / 'game'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(param_sfo({'TITLE_ID': 'CUSA18758', 'CATEGORY': 'gd'}))
        self.user = self.root / 'user'

    def tearDown(self):
        self.tmp.cleanup()

    def test_unlock_key_packages_become_entitlements(self):
        for label in ('GUNDAMVSMBDLC001', 'GUNDAMVSMBDLC002'):
            unlock_key(self.dlc / f'UP0700-CUSA18758_00-{label}-A0000-V0100.pkg',
                       f'UP0700-CUSA18758_00-{label}')
        unlock_key(self.dlc / 'other.pkg', 'UP9000-CUSA00900_00-SPEXPANSIONDLC03')  # another game
        unlock_key(self.dlc / 'patch.pkg', 'UP0700-CUSA18758_00-EXTREMEVSMBONPKG', 'gp')  # not an add-on
        (self.dlc / 'notes.pkg').write_bytes(b'not a package')
        self.assertEqual(sorted(dlc.addons(self.dlc, 'CUSA18758')), ['GUNDAMVSMBDLC001', 'GUNDAMVSMBDLC002'])
        self.assertEqual(dlc.link(self.dlc, self.game, self.user), 2)
        target = self.user / 'addcont/CUSA18758'
        self.assertTrue((target / 'GUNDAMVSMBDLC001').is_dir())
        self.assertTrue((target / 'GUNDAMVSMBDLC002').is_dir())
        # Rebuilt at the next launch: an add-on taken out of the folder is gone.
        (self.dlc / 'UP0700-CUSA18758_00-GUNDAMVSMBDLC002-A0000-V0100.pkg').unlink()
        self.assertEqual(dlc.link(self.dlc, self.game, self.user), 1)
        self.assertFalse((target / 'GUNDAMVSMBDLC002').exists())

    def test_dumped_add_on_folders_are_linked(self):
        folder = self.dlc / 'GUNDAMVSMBDLC003'
        (folder / 'sce_sys').mkdir(parents=True)
        (folder / 'sce_sys/param.sfo').write_bytes(param_sfo({
            'CATEGORY': 'ac', 'CONTENT_ID': 'UP0700-CUSA18758_00-GUNDAMVSMBDLC003', 'TITLE_ID': 'CUSA18758'}))
        # The link an earlier run.sh made to the whole folder is replaced.
        (self.user / 'addcont').mkdir(parents=True)
        (self.user / 'addcont/CUSA18758').symlink_to(self.dlc)
        self.assertEqual(dlc.link(self.dlc, self.game, self.user), 1)
        entry = self.user / 'addcont/CUSA18758/GUNDAMVSMBDLC003'
        self.assertTrue(entry.is_symlink())
        self.assertEqual(entry.resolve(), folder.resolve())

    def test_a_folder_of_your_own_is_left_alone(self):
        unlock_key(self.dlc / 'a.pkg', 'UP0700-CUSA18758_00-GUNDAMVSMBDLC001')
        own = self.user / 'addcont/CUSA18758/SOMETHINGELSE001'
        own.mkdir(parents=True)
        self.assertEqual(dlc.link(self.dlc, self.game, self.user), 0)
        self.assertTrue(own.is_dir())
        self.assertFalse((self.user / 'addcont/CUSA18758/GUNDAMVSMBDLC001').exists())


if __name__ == '__main__':
    unittest.main()
