/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Use DroidDeck's narrow Steam/PRoot status fix without its game/GPU/input hooks. */
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
