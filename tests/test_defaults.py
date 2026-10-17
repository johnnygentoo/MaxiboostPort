from paths import ROOT
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_defaults  # noqa: E402

BUILTIN = {'game_dir': '/default/CUSA02046', 'trophy_key': '', 'fullscreen': False, 'hdr': False,
           'present_mode': 'Mailbox', 'fps_limit': 0, 'save_log': False}
BUILTIN_INI = {'upscaler': 'fsr4', 'preset': '4', 'output_res': '1920x1080', 'show_fps': '1'}
KEEP = ('game_dir', 'trophy_key')


class RestoreTests(unittest.TestCase):
    def setUp(self):
        self.current = {'game_dir': '/games/gz', 'trophy_key': 'ab' * 16, 'fullscreen': False,
                        'hdr': False, 'present_mode': 'Fifo', 'fps_limit': 30, 'save_log': False,
                        'fsr411_dll_dir': '/dlls'}

    def test_defaults_go_over_the_current_settings(self):
        defaults = {'launcher': {'fullscreen': True, 'hdr': True, 'present_mode': 'Mailbox'},
                    'ini': {'upscaler': 'off', 'output_res': '3840x2160'}}
        settings, ini = bbport_defaults.restore(self.current, defaults, BUILTIN, BUILTIN_INI, KEEP)
        self.assertEqual(settings, {**self.current, 'fullscreen': True, 'hdr': True,
                                    'present_mode': 'Mailbox'})
        self.assertEqual(ini, {'upscaler': 'off', 'output_res': '3840x2160'})
        self.assertEqual(self.current['present_mode'], 'Fifo')  # the caller's dict is not changed

    def test_defaults_may_set_the_kept_keys(self):
        defaults = {'launcher': {'game_dir': '/steam/gz', 'trophy_key': ''}, 'ini': {}}
        settings, _ = bbport_defaults.restore(self.current, defaults, BUILTIN, BUILTIN_INI, KEEP)
        self.assertEqual((settings['game_dir'], settings['trophy_key']), ('/steam/gz', ''))

    def test_values_of_another_type_are_skipped(self):
        defaults = {'launcher': {'fullscreen': 'yes', 'fps_limit': 60.5, 'hdr': True, 'new_key': 1},
                    'ini': {}}
        settings, _ = bbport_defaults.restore(self.current, defaults, BUILTIN, BUILTIN_INI, KEEP)
        self.assertIs(settings['fullscreen'], False)
        self.assertEqual(settings['fps_limit'], 30)
        self.assertIs(settings['hdr'], True)
        self.assertEqual(settings['new_key'], 1)

    def test_without_a_file_the_builtin_defaults_keep_folders_and_key(self):
        settings, ini = bbport_defaults.restore(self.current, None, BUILTIN, BUILTIN_INI, KEEP)
        self.assertEqual(settings['game_dir'], '/games/gz')
        self.assertEqual(settings['trophy_key'], 'ab' * 16)
        self.assertEqual(settings['present_mode'], 'Mailbox')
        self.assertEqual(settings['fps_limit'], 0)
        self.assertEqual(settings['fsr411_dll_dir'], '/dlls')
        self.assertEqual(ini, BUILTIN_INI)


class IniLinesTests(unittest.TestCase):
    LINES = ['# bbport settings (in-game menu: Insert / L3+R3)', 'upscaler=fsr4', 'preset=4',
             'key.cross=Space, Mouse Left', 'pad.cross=b', 'pad.circle=a', '# a comment',
             'menu_language=de', 'output_res=1920x1080']

    def test_named_keys_are_rewritten_and_the_rest_kept(self):
        out = bbport_defaults.ini_lines({'upscaler': 'off', 'output_res': '3840x2160',
                                         'skip_intro': '0'}, self.LINES)
        self.assertEqual(out, ['# bbport settings (in-game menu: Insert / L3+R3)', 'upscaler=off',
                               'preset=4', 'key.cross=Space, Mouse Left', 'pad.cross=b',
                               'pad.circle=a', '# a comment', 'menu_language=de',
                               'output_res=3840x2160', 'skip_intro=0'])

    def test_remaps_change_only_when_named(self):
        out = bbport_defaults.ini_lines({'pad.cross': None, 'pad.circle': None, 'key.cross': 'Return'},
                                        self.LINES)
        self.assertIn('key.cross=Return', out)
        self.assertFalse([line for line in out if line.startswith(('pad.cross', 'pad.circle'))])
        self.assertIn('menu_language=de', out)

    def test_an_empty_file_gets_its_header(self):
        self.assertEqual(bbport_defaults.ini_lines({'upscaler': 'off', 'pad.cross': None}, []),
                         ['# bbport settings (in-game menu: Insert / L3+R3)', 'upscaler=off'])


class FileTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / 'gzport-launcher' / 'defaults.json'

    def test_missing_file_is_none(self):
        self.assertIsNone(bbport_defaults.load(self.path))

    def test_save_and_load(self):
        bbport_defaults.save(self.path, {'fullscreen': True, 'trophy_key': 'k'}, {'upscaler': 'off'})
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(bbport_defaults.load(self.path),
                         {'launcher': {'fullscreen': True, 'trophy_key': 'k'}, 'ini': {'upscaler': 'off'}})

    def test_ini_values_are_text_and_parts_optional(self):
        self.path.parent.mkdir(parents=True)
        self.path.write_text(json.dumps({'ini': {'preset': 4, 'pad.cross': None}}))
        self.assertEqual(bbport_defaults.load(self.path),
                         {'launcher': {}, 'ini': {'preset': '4', 'pad.cross': None}})

    def test_a_broken_file_raises(self):
        self.path.parent.mkdir(parents=True)
        for text in ('{', '[]', '{"launcher": []}'):
            self.path.write_text(text)
            with self.subTest(text=text), self.assertRaises(ValueError):
                bbport_defaults.load(self.path)


if __name__ == '__main__':
    unittest.main()
