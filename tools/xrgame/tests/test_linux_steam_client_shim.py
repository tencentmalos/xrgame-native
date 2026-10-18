# SPDX-License-Identifier: GPL-3.0-or-later
"""The client shim's web helper GL switch, built for the host and run against a stand-in."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
SHIMS = [REPO / 'references/DroidDeck/tools/linuxfs/preload' / name
         for name in ('sysv.c', 'net.c', 'udevmon.c', 'binfmt.c', 'tracer.c', 'lock.c')]
NAMES = ('__GLX_VENDOR_LIBRARY_NAME', 'GALLIUM_DRIVER', 'MESA_LOADER_DRIVER_OVERRIDE', 'LIBGL_KOPPER_DRI2',
         'LIBGL_ALWAYS_SOFTWARE', 'LIBGL_KOPPER_DISABLE', 'XRGAME_X11_HOST_DISPLAY')
PROGRAM = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *names[] = {%s};
int main(int argc, char **argv) {
    const char *role = argc > 1 && strncmp(argv[1], "--type=", 7) == 0 ? "child" : "browser";
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
        const char *value = getenv(names[i]);
        printf("%%s %%s=%%s\n", role, names[i], value ? value : "<unset>");
    }
    fflush(stdout);
    if (argc > 1 && strcmp(argv[1], "--spawn") == 0) {
        char *child[] = {argv[0], "--type=renderer", NULL};
        execv(argv[0], child);
    }
    return 0;
}
''' % ', '.join(f'"{name}"' for name in NAMES)
CLIENT = {'LIBGL_ALWAYS_SOFTWARE': '1', 'GALLIUM_DRIVER': 'llvmpipe', 'LIBGL_KOPPER_DISABLE': 'true',
          'XRGAME_X11_HOST_DISPLAY': ':0'}


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('gcc') and all(p.is_file() for p in SHIMS),
                     'needs Linux, gcc and the DroidDeck reference')
class WebHelperGlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp())
        cls.shim = cls.work / 'libshim.so'
        subprocess.run(['gcc', '-O1', '-shared', '-fPIC', '-pthread', *map(str, SHIMS),
                        str(REPO / 'tools/xrgame/linux-steam/client-proc.c'), '-ldl', '-o', str(cls.shim)],
                       check=True)
        (cls.work / 'probe.c').write_text(PROGRAM)
        for name in ('steamwebhelper', 'steam'):
            subprocess.run(['gcc', '-O1', str(cls.work / 'probe.c'), '-o', str(cls.work / name)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def run_probe(self, program, extra):
        env = dict(os.environ, LD_PRELOAD=str(self.shim), **CLIENT, **extra)
        env.pop('__GLX_VENDOR_LIBRARY_NAME', None)
        env.pop('LIBGL_KOPPER_DRI2', None)
        out = subprocess.run([str(self.work / program), '--spawn'], env=env, check=True,
                             capture_output=True, text=True).stdout
        roles = {}
        for line in out.splitlines():
            role, pair = line.split(' ', 1)
            name, value = pair.split('=', 1)
            roles.setdefault(role, {})[name] = value
        return roles

    def test_the_web_helper_tree_draws_with_the_vendor(self):
        roles = self.run_probe('steamwebhelper', {'XRGAME_WEBHELPER_GL_VENDOR': 'steampsp'})
        expected = {'__GLX_VENDOR_LIBRARY_NAME': 'steampsp', 'GALLIUM_DRIVER': 'zink',
                    'MESA_LOADER_DRIVER_OVERRIDE': 'zink', 'LIBGL_KOPPER_DRI2': 'true',
                    'LIBGL_ALWAYS_SOFTWARE': '<unset>', 'LIBGL_KOPPER_DISABLE': '<unset>',
                    'XRGAME_X11_HOST_DISPLAY': ':0'}
        # Chromium's children inherit the browser's environment; the shim leaves them alone.
        self.assertEqual(expected, roles['browser'])
        self.assertEqual(expected, roles['child'])

    def test_without_the_switch_nothing_changes(self):
        roles = self.run_probe('steamwebhelper', {})
        for role in ('browser', 'child'):
            self.assertEqual('llvmpipe', roles[role]['GALLIUM_DRIVER'])
            self.assertEqual(':0', roles[role]['XRGAME_X11_HOST_DISPLAY'])
            self.assertEqual('<unset>', roles[role]['__GLX_VENDOR_LIBRARY_NAME'])

    def test_other_programs_are_untouched(self):
        roles = self.run_probe('steam', {'XRGAME_WEBHELPER_GL_VENDOR': 'steampsp'})
        self.assertEqual('llvmpipe', roles['browser']['GALLIUM_DRIVER'])
        self.assertEqual('<unset>', roles['browser']['__GLX_VENDOR_LIBRARY_NAME'])


if __name__ == '__main__':
    unittest.main()
