import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

path = Path(__file__).parents[1] / 'linux-steam/launch-client.py'
spec = importlib.util.spec_from_file_location('linux_steam_launcher', path)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class ClientDisplayLifecycleTest(unittest.TestCase):
    def test_first_screen_waits_for_cef_not_updater(self):
        self.assertFalse(launcher.client_window_ready('0x1 "更新 Steam" ("Steam" "Steam") 640x480+0+0'))
        self.assertFalse(launcher.client_window_ready('0x1 "steamwebhelper" ("steamwebhelper" "Steamwebhelper") 10x10+0+0'))
        self.assertTrue(launcher.client_window_ready('0x1 "Steam 大屏幕模式" ("steamwebhelper" "steam") 1280x720+0+0'))
        self.assertTrue(launcher.client_window_ready('0x1 "Sign in" ("steamwebhelper" "steam") 700x500+0+0'))

    def test_device_ratio_reaches_xephyr(self):
        self.assertEqual('1280x720', launcher.screen_size('1280x720'))
        for invalid in ('0x800', '1280x999999', '1280x720 -ac', 'oops'):
            with self.subTest(size=invalid), self.assertRaises(ValueError):
                launcher.screen_size(invalid)

    def test_update_restart_code_and_display_cleanup(self):
        display = Mock()
        display.poll.return_value = None
        client = Mock(returncode=42)
        client.poll.return_value = 42
        with patch.object(launcher.sys, 'argv', ['launch-client.py', '/steam', '-nobigpicture']), \
             patch.object(launcher.signal, 'signal'), \
             patch.object(launcher, 'session_bus', return_value=(Mock(), 'unix:abstract=steampsp-test')), \
             patch.object(launcher.subprocess, 'Popen', side_effect=[display, client]) as start, \
             patch.object(launcher.subprocess, 'run', return_value=Mock(returncode=0)), \
             patch.dict(launcher.os.environ, {'DISPLAY': ':0', 'LD_LIBRARY_PATH': '/valve', 'XRGAME_STEAM_SCREEN': '1280x720'}):
            self.assertEqual(42, launcher.main())
        display_args = start.call_args_list[0].args[0]
        self.assertEqual('1280x720', display_args[display_args.index('-screen') + 1])
        self.assertEqual(':0', start.call_args_list[0].kwargs['env']['DISPLAY'])
        self.assertNotIn('LD_LIBRARY_PATH', start.call_args_list[0].kwargs['env'])
        self.assertEqual(':1', start.call_args_list[1].kwargs['env']['DISPLAY'])
        self.assertEqual('/valve', start.call_args_list[1].kwargs['env']['LD_LIBRARY_PATH'])
        # launch-client.py owns the session bus: no dbus-run-session process stays resident.
        self.assertEqual('unix:abstract=steampsp-test', start.call_args_list[1].kwargs['env']['DBUS_SESSION_BUS_ADDRESS'])
        display.terminate.assert_called_once()
        display.wait.assert_called_once()
        client.terminate.assert_not_called()

    def test_failed_display_never_starts_client(self):
        display = Mock()
        display.poll.return_value = 1
        with patch.object(launcher.sys, 'argv', ['launch-client.py', '/steam']), \
             patch.object(launcher.signal, 'signal'), \
             patch.object(launcher, 'session_bus', return_value=(Mock(), 'unix:abstract=steampsp-test')), \
             patch.object(launcher.subprocess, 'Popen', return_value=display) as start:
            self.assertEqual(70, launcher.main())
        start.assert_called_once()
        display.wait.assert_called_once()

    def test_interruption_during_startup_reaps_display(self):
        display = Mock()
        display.poll.return_value = None
        with patch.object(launcher.sys, 'argv', ['launch-client.py', '/steam']), \
             patch.object(launcher.signal, 'signal'), \
             patch.object(launcher, 'session_bus', return_value=(Mock(), 'unix:abstract=steampsp-test')), \
             patch.object(launcher.subprocess, 'Popen', return_value=display), \
             patch.object(launcher.subprocess, 'run', side_effect=SystemExit(143)):
            with self.assertRaises(SystemExit):
                launcher.main()
        display.terminate.assert_called_once()
        display.wait.assert_called_once()


class GpuDrawingTest(unittest.TestCase):
    def start(self, available):
        display = Mock()
        display.poll.return_value = None
        client = Mock(returncode=0)
        client.poll.return_value = 0
        with patch.object(launcher.sys, 'argv', ['launch-client.py', '/steam']), \
             patch.object(launcher.signal, 'signal'), \
             patch.object(launcher, 'session_bus', return_value=(Mock(), 'unix:abstract=steampsp-test')), \
             patch.object(launcher, 'gpu_available', return_value=available), \
             patch.object(launcher.subprocess, 'Popen', side_effect=[display, client]) as start, \
             patch.object(launcher.subprocess, 'run', return_value=Mock(returncode=0)), \
             patch.dict(launcher.os.environ, {'LD_LIBRARY_PATH': '/valve', 'XRGAME_STEAM_GPU': '1'}):
            self.assertEqual(0, launcher.main())
        return start.call_args_list[0].kwargs['env'], start.call_args_list[1].kwargs['env']

    def test_only_the_client_presents_to_the_android_server(self):
        xephyr, client = self.start(available=True)
        self.assertEqual(':1', client['DISPLAY'])
        self.assertEqual(':0', client['XRGAME_X11_HOST_DISPLAY'])
        self.assertEqual(launcher.GPU_ICD, client['VK_ICD_FILENAMES'])
        self.assertEqual('steampsp', client['XRGAME_WEBHELPER_GL_VENDOR'])
        self.assertEqual('/opt/steampsp-gpu/lib:/valve', client['LD_LIBRARY_PATH'])
        # VGUI keeps llvmpipe GLX inside Xephyr; Xephyr itself gets no GPU settings.
        self.assertEqual('llvmpipe', client['GALLIUM_DRIVER'])
        for name in ('XRGAME_X11_HOST_DISPLAY', 'VK_ICD_FILENAMES', 'XRGAME_STEAM_GPU', 'XRGAME_WEBHELPER_GL_VENDOR'):
            self.assertNotIn(name, xephyr)

    def test_missing_component_keeps_software_drawing(self):
        _, client = self.start(available=False)
        self.assertNotIn('XRGAME_X11_HOST_DISPLAY', client)
        self.assertNotIn('VK_ICD_FILENAMES', client)
        self.assertNotIn('XRGAME_WEBHELPER_GL_VENDOR', client)
        self.assertEqual('/valve', client['LD_LIBRARY_PATH'])


if __name__ == '__main__':
    unittest.main()
