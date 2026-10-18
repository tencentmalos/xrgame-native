/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Use DroidDeck's narrow Steam/PRoot status fix without its game/GPU/input hooks.
 * GPU drawing adds one opt-in environment switch for Steam's web helper (end of file). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
/* Opt-in, bounded filesystem diagnostics. Never log file contents or userdata. */
static void trace_library(const char *api, const char *path, long result, int error) {
    static unsigned count;
    if (!getenv("XRGAME_STEAM_LIBRARY_TRACE") || !path || strcmp(program_invocation_short_name, "steam") != 0) return;
    const char *metadata = strstr(path, "/steamapps/");
    if (!metadata || (strncmp(metadata + 11, "appmanifest_", 12) != 0 &&
        strncmp(metadata + 11, "libraryfolders.vdf", 18) != 0) ||
        __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 128) return;
    int saved = errno;
    fprintf(stderr, "SteamPSP library: %s %s result=%ld errno=%d\n", api, path, result, error);
    errno = saved;
}
int bl_status_without_tracer(const char *, int) __attribute__((visibility("hidden")));
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    int fd = bl_status_without_tracer(path, flags);
    if (fd >= 0) return fd;
    int (*real)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
    int result = real(path, flags, mode);
    trace_library("open", path, result, result < 0 ? errno : 0);
    return result;
}
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    return open(path, flags | O_LARGEFILE, mode);
}
static FILE *status_file(const char *path, const char *mode, const char *function) {
    if (mode && mode[0] == 'r' && mode[1] == 0) {
        int fd = bl_status_without_tracer(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { FILE *result = fdopen(fd, mode); if (!result) close(fd); return result; }
    }
    FILE *(*real)(const char *, const char *) = dlsym(RTLD_NEXT, function);
    FILE *result = real(path, mode);
    trace_library(mode, path, result != NULL, result ? 0 : errno);
    return result;
}
FILE *fopen(const char *path, const char *mode) { return status_file(path, mode, "fopen"); }
FILE *fopen64(const char *path, const char *mode) { return status_file(path, mode, "fopen64"); }
/*
 * GPU drawing (XRGAME_WEBHELPER_GL_VENDOR): Steam's web helper draws with that glvnd vendor's
 * Zink: CEF's GPU process and Steam's compositor alike. Steam sets LIBGL_KOPPER_DISABLE for
 * its web helper, and Zink needs kopper. Only the browser process, which Steam starts, needs
 * the change; Chromium starts its children (through /proc/self/exe) with its environment.
 */
__attribute__((constructor)) static void webhelper_gl(int argc, char **argv) {
    const char *vendor = getenv("XRGAME_WEBHELPER_GL_VENDOR");
    if (!vendor || !*vendor || strcmp(program_invocation_short_name, "steamwebhelper") != 0) return;
    for (int i = 1; i < argc; i++) if (argv[i] && strncmp(argv[i], "--type=", 7) == 0) return;
    setenv("__GLX_VENDOR_LIBRARY_NAME", vendor, 1);
    setenv("GALLIUM_DRIVER", "zink", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("LIBGL_KOPPER_DRI2", "true", 1);
    unsetenv("LIBGL_ALWAYS_SOFTWARE");
    unsetenv("LIBGL_KOPPER_DISABLE");
}
