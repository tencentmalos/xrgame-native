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


ARGV_PROGRAM = r'''
#include <stdio.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) printf("%s\n", argv[i]);
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('gcc') and all(p.is_file() for p in SHIMS),
                     'needs Linux, gcc and the DroidDeck reference')
class FewerProcessesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp())
        cls.shim = cls.work / 'libshim.so'
        subprocess.run(['gcc', '-O1', '-shared', '-fPIC', '-pthread', *map(str, SHIMS),
                        str(REPO / 'tools/xrgame/linux-steam/client-proc.c'), '-ldl', '-o', str(cls.shim)], check=True)
        (cls.work / 'argv.c').write_text(ARGV_PROGRAM)
        for name in ('steamwebhelper', 'steam'):
            subprocess.run(['gcc', '-O1', str(cls.work / 'argv.c'), '-o', str(cls.work / name)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def arguments(self, program, *arguments, enabled=True, fold='1', instant=None):
        env = dict(os.environ, LD_PRELOAD=str(self.shim))
        env.pop('XRGAME_WEBHELPER_REEXEC', None)
        if enabled:
            env['XRGAME_WEBHELPER_FEWER_PROCESSES'] = fold
        if instant is not None:
            env['XRGAME_WEBHELPER_INSTANT_SCROLL'] = instant
        return subprocess.run([str(self.work / program), *arguments], env=env, check=True,
                              capture_output=True, text=True).stdout.splitlines()

    def test_the_browser_process_restarts_with_merged_features(self):
        steam = ['-lang=en', '--enable-features=PlatformHEVCDecoderSupport', '--disable-features=SpareRendererForSitePerProcess']
        self.assertEqual(['-lang=en', '--enable-features=PlatformHEVCDecoderSupport,NetworkServiceInProcess2',
                          '--disable-features=SpareRendererForSitePerProcess,AudioServiceOutOfProcess', '--no-zygote'],
                         self.arguments('steamwebhelper', *steam))

    def test_missing_feature_lists_are_added(self):
        self.assertEqual(['-lang=en', '--no-zygote', '--enable-features=NetworkServiceInProcess2',
                          '--disable-features=AudioServiceOutOfProcess'], self.arguments('steamwebhelper', '-lang=en'))

    def test_only_the_chosen_processes_are_folded(self):
        steam = ['--enable-features=PlatformHEVCDecoderSupport', '--disable-features=SpareRendererForSitePerProcess']
        self.assertEqual(['--enable-features=PlatformHEVCDecoderSupport',
                          '--disable-features=SpareRendererForSitePerProcess,AudioServiceOutOfProcess', '--no-zygote'],
                         self.arguments('steamwebhelper', *steam, fold='zygote,audio'))
        self.assertEqual(steam, self.arguments('steamwebhelper', *steam, fold=''))

    def test_instant_scrolling_replaces_smooth_scrolling(self):
        steam = ['-lang=en', '--enable-smooth-scrolling']
        self.assertEqual(['-lang=en', '--disable-smooth-scrolling'],
                         self.arguments('steamwebhelper', *steam, fold='', instant='1'))
        self.assertEqual(['-lang=en', '--disable-smooth-scrolling'],
                         self.arguments('steamwebhelper', *steam, enabled=False, instant='1'))
        self.assertEqual(steam, self.arguments('steamwebhelper', *steam, fold='', instant='0'))
        self.assertEqual(['--type=renderer', '--enable-smooth-scrolling'],
                         self.arguments('steamwebhelper', '--type=renderer', '--enable-smooth-scrolling', instant='1'))

    def test_children_other_programs_and_the_opt_out_keep_their_arguments(self):
        self.assertEqual(['--type=renderer'], self.arguments('steamwebhelper', '--type=renderer'))
        self.assertEqual(['-lang=en'], self.arguments('steam', '-lang=en'))
        self.assertEqual(['-lang=en'], self.arguments('steamwebhelper', '-lang=en', enabled=False))


# Android 16's app seccomp policy, as Xephyr meets it: setuid/setgid fail with ENOSYS.
SETID_PROGRAM = r'''
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setuid, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setgid, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
    };
    struct sock_fprog program = {sizeof(filter) / sizeof(*filter), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) return 2;
    int gid = setgid(getgid()), uid = setuid(getuid());
    printf("unchanged %d %d\n", gid, uid);
    errno = 0;
    int other = setuid(getuid() + 1);
    printf("changed %d %d\n", other, errno);
    return 0;
}
'''


# Android 16's refusal of hard links in app data, as Xephyr's lock file meets it.
LINK_PROGRAM = r'''
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_link, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_linkat, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    };
    struct sock_fprog program = {sizeof(filter) / sizeof(*filter), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) return 2;
    errno = 0;
    int first = link(argv[1], argv[2]);
    printf("first %d %d\n", first, errno);
    errno = 0;
    int again = link(argv[1], argv[2]);
    printf("again %d %d\n", again, errno);
    return 0;
}
'''


# Steam's IPC mutexes ask for the thread's robust futex list through libc syscall().
ROBUST_PROGRAM = r'''
#include <errno.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
static void *ask(void *out) {
    struct robust_list_head *head = NULL;
    size_t size = 0;
    errno = 0;
    long result = syscall(SYS_get_robust_list, 0, &head, &size);
    // glibc's empty list: next points at the head, and the word before it (robust_prev) too.
    printf("result %ld errno %d size %zu offset %ld empty %d\n", result, errno, size,
           head ? head->futex_offset : 0L,
           head && head->list.next == &head->list && ((void **)head)[-1] == (void *)head);
    *(void **)out = head;
    return NULL;
}
int main(void) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_get_robust_list, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_set_robust_list, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
    };
    struct sock_fprog program = {sizeof(filter) / sizeof(*filter), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) return 2;
    void *mine = NULL, *other = NULL;
    ask(&mine);
    pthread_t thread;
    pthread_create(&thread, NULL, ask, &other);
    pthread_join(thread, NULL);
    printf("per-thread %d\n", mine != other);
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('gcc') and all(p.is_file() for p in SHIMS),
                     'needs Linux, gcc and the DroidDeck reference')
class AndroidSixteenTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp())
        cls.shim = cls.work / 'libshim.so'
        subprocess.run(['gcc', '-O1', '-shared', '-fPIC', '-pthread', *map(str, SHIMS),
                        str(REPO / 'tools/xrgame/linux-steam/client-proc.c'), '-ldl', '-o', str(cls.shim)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def build(self, name, source):
        (self.work / f'{name}.c').write_text(source)
        subprocess.run(['gcc', '-O1', '-pthread', str(self.work / f'{name}.c'), '-o', str(self.work / name)], check=True)
        return self.work / name

    def test_a_refused_robust_list_query_gets_an_empty_glibc_head_per_thread(self):
        plain, shimmed = self.run_both(self.build('robust', ROBUST_PROGRAM))
        self.assertIn('result -1 errno 38', plain)
        lines = shimmed.splitlines()
        self.assertEqual(2, sum(line.startswith('result 0 errno 0 size 24 offset -32 empty 1') for line in lines), shimmed)
        self.assertIn('per-thread 1', shimmed)

    def run_both(self, program, *arguments, before=None):
        results = []
        for env in (os.environ, dict(os.environ, LD_PRELOAD=str(self.shim))):
            if before:
                before()
            results.append(subprocess.run([str(program), *arguments], env=env, check=True,
                                          capture_output=True, text=True).stdout)
        return results

    def test_unchanged_ids_succeed_and_changes_reach_the_kernel(self):
        plain, shimmed = self.run_both(self.build('setid', SETID_PROGRAM))
        self.assertIn('unchanged -1 -1', plain)
        self.assertIn('unchanged 0 0', shimmed)
        self.assertIn('changed -1 38', shimmed)

    def test_a_refused_hard_link_becomes_an_exclusive_copy(self):
        source, lock = self.work / '.tX1-lock', self.work / '.X1-lock'
        source.write_text('      4242\n')

        def reset():
            lock.unlink(missing_ok=True)
        plain, shimmed = self.run_both(self.build('link', LINK_PROGRAM), str(source), str(lock), before=reset)
        self.assertIn(f'first -1 {1}', plain)
        self.assertIn('first 0 0', shimmed)
        self.assertIn(f'again -1 {17}', shimmed)  # EEXIST, as a second link would fail
        self.assertEqual('      4242\n', lock.read_text())


if __name__ == '__main__':
    unittest.main()
