from paths import ROOT  # noqa: F401
import subprocess
import sys
import unittest

import discord_presence as presence


class ActivityTests(unittest.TestCase):
    def test_the_game_and_the_time_played(self):
        value = presence.activity(100)
        self.assertEqual(value['details'], 'GUNDAM VERSUS')
        self.assertEqual(value['timestamps'], {'start': 100})
        self.assertNotIn('Bloodborne', str(value))


class ClientIdTests(unittest.TestCase):
    def test_without_an_application_id_nothing_is_shown(self):
        script = ROOT / 'scripts/discord_presence.py'
        run = subprocess.run([sys.executable, str(script), '--pid', '1', '--client-id', ''],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0)
        self.assertIn('Rich Presence off', run.stdout)


if __name__ == '__main__':
    unittest.main()
