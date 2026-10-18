import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location("game_launch", Path(__file__).parents[1] / "linux-steam/game-launch.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class HandoffTest(unittest.TestCase):
    def test_invalid_ids_and_probes_never_launch(self):
        for app in ("0", "-1", "../42", "2147483648"):
            with self.assertRaises(ValueError):
                module.app_id({"SteamAppId": app})
        self.assertEqual(1, module.main(["run", "/tmp/d3ddriverquery.exe"], {"SteamAppId": "42"}))

    def test_autocloud_path_reaches_same_existing_save(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'original'
            user = prefix / 'drive_c/users/xuser'
            save = user / 'Saved Games/Hades II/Profile1.sav'
            save.parent.mkdir(parents=True)
            save.write_text('keep')
            (user.parent / 'steamuser').symlink_to(user, target_is_directory=True)
            compat = root / 'compatdata/42'
            compat.mkdir(parents=True)
            (compat / 'pfx').symlink_to(prefix, target_is_directory=True)
            env = {'STEAM_COMPAT_DATA_PATH': str(compat)}
            native = module.translate_path('getnativepath', r'C:\users\steamuser\Saved Games\Hades II\Profile1.sav', env)
            self.assertEqual(save.resolve(), Path(native))
            self.assertEqual('keep', Path(native).read_text())
            windows = module.translate_path('getcompatpath', native, env)
            self.assertEqual(save.resolve(), Path(module.translate_path('getnativepath', windows, env)))
            unix = root / 'external/game.exe'
            windows = module.translate_path('getcompatpath', str(unix), env)
            self.assertEqual(unix.resolve(), Path(module.translate_path('getnativepath', windows, env)))

    def test_path_probe_does_not_create_an_independent_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            env = {'STEAM_COMPAT_DATA_PATH': directory}
            with self.assertRaises(ValueError):
                module.translate_path('getnativepath', r'C:\users\steamuser', env)
            self.assertEqual([], list(Path(directory).iterdir()))
            with self.assertRaises(ValueError):
                module.translate_path('getnativepath', 'bad\npath', env)

    def test_timeout_cleans_up_without_creating_a_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(1, module.launch(directory, 42, timeout=0))
            self.assertEqual([], list(Path(directory).iterdir()))

    def test_steam_command_stays_inside_the_game(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory) / 'common/Hades II'
            (game / 'Ship').mkdir(parents=True)
            env = {'STEAM_COMPAT_INSTALL_PATH': str(game)}
            exe = str(game / 'Ship/Hades2.exe')
            self.assertEqual({'executable': 'Ship/Hades2.exe', 'arguments': ['/c', '../', 'a b'], 'workingDirectory': 'Ship'},
                             module.steam_command(['waitforexitandrun', exe, '/c', '../', 'a b'], env, str(game / 'Ship')))
            # A working directory outside the game is left to Android.
            self.assertEqual('', module.steam_command(['run', exe], env, directory)['workingDirectory'])
            for outside in (str(Path(directory) / 'other.exe'), str(game / '../escape.exe'), str(game)):
                self.assertIsNone(module.steam_command(['run', outside], env, str(game)))
            self.assertIsNone(module.steam_command(['run', exe], {}, str(game)))
            self.assertIsNone(module.steam_command(['run', exe] + ['x'] * 65, env, str(game)))
            self.assertIsNone(module.steam_command(['run', exe, 'a\x00b'], env, str(game)))

    def test_oversized_command_falls_back_to_the_id(self):
        body = json.loads(module.request_body(42, {'executable': 'a.exe', 'arguments': ['x' * 20000], 'workingDirectory': ''}))
        self.assertEqual({'appId', 'pid'}, set(body))

    def test_waits_for_session_exit_and_sends_the_steam_command(self):
        command = {'executable': 'Ship/Hades2.exe', 'arguments': ['/c', '../'], 'workingDirectory': 'Ship'}
        with tempfile.TemporaryDirectory() as directory:
            result = []
            worker = threading.Thread(target=lambda: result.append(module.launch(directory, 42, timeout=3, command=command)))
            worker.start()
            try:
                deadline = time.monotonic() + 3
                requests = []
                while not requests and time.monotonic() < deadline:
                    requests = list(Path(directory).glob("*.request")); time.sleep(.01)
                self.assertEqual(1, len(requests))
                request = requests[0]
                body = json.loads(request.read_text())
                self.assertEqual({"appId", "pid", "command"}, set(body))
                self.assertEqual(command, body["command"])
                response = request.with_suffix(".result")
                response.write_text('{"state":"running"}')
                time.sleep(.6)
                self.assertTrue(worker.is_alive())
                temp = request.with_suffix(".tmp"); temp.write_text('{"state":"finished"}'); temp.replace(response)
                worker.join(3)
                self.assertEqual([0], result)
                self.assertEqual([], list(Path(directory).iterdir()))
            finally:
                worker.join(4)


if __name__ == "__main__":
    unittest.main()
