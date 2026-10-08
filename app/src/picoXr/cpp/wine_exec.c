// SPDX-License-Identifier: GPL-3.0-or-later
// Clean-room adapter for Wine's execv/execve and posix_spawn calls on Android.
// No code from the proprietary redirect libraries is used.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char **environ;
static const char linker[] = "/system/bin/linker64";
static char runtime_root[PATH_MAX];
static int (*spawn_next)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                         const posix_spawnattr_t *, char *const[], char *const[]);

__attribute__((constructor)) static void initialize(void) {
    const char *root = getenv("XRGAME_EXEC_ROOT");
    if (root && root[0] == '/' && realpath(root, runtime_root)) {
        if (!strcmp(runtime_root, "/")) runtime_root[0] = 0;
    } else runtime_root[0] = 0;
    spawn_next = dlsym(RTLD_NEXT, "posix_spawn");
}

// Only app-owned, executable, dynamic AArch64 ELF files beneath our runtime
// directory qualify. Scripts, static Wine preloaders and system programs keep
// their original result. In particular, Wine already falls back from its static
// preloader to the dynamic loader when the former cannot be executed.
static bool can_link(const char *path, char *const argv[]) {
    if (!runtime_root[0] || !path || path[0] != '/' || !argv || !argv[0] ||
        strcmp(argv[0], path)) return false;
    // Android's linker has no --argv0: limit adaptation to Wine's path-as-argv0
    // convention rather than silently changing a caller's custom argv[0].
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = false;
    struct stat st;
    Elf64_Ehdr eh;
    char fdpath[64], resolved[PATH_MAX];
    // Avoid allocation / stdio / lazy dlsym in the post-fork exec path.
    const char prefix[] = "/proc/self/fd/";
    memcpy(fdpath, prefix, sizeof(prefix) - 1);
    char digits[16];
    unsigned n = 0, value = (unsigned)fd;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    unsigned at = sizeof(prefix) - 1;
    while (n) fdpath[at++] = digits[--n];
    fdpath[at] = 0;
    ssize_t length = readlink(fdpath, resolved, sizeof(resolved) - 1);
    if (length < 0) goto done;
    resolved[length] = 0;
    size_t root_length = strlen(runtime_root);
    if (strncmp(resolved, runtime_root, root_length) || resolved[root_length] != '/' ||
        fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        !(st.st_mode & S_IXUSR) || (st.st_mode & (S_ISUID | S_ISGID))) goto done;
    if (pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_AARCH64 ||
        eh.e_type != ET_DYN || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        eh.e_phnum > 256 || eh.e_phoff > (Elf64_Off)st.st_size ||
        (Elf64_Off)eh.e_phnum * sizeof(Elf64_Phdr) > (Elf64_Off)st.st_size - eh.e_phoff)
        goto done;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (pread(fd, &ph, sizeof(ph), eh.e_phoff + i * sizeof(ph)) != sizeof(ph)) break;
        if (ph.p_type != PT_INTERP) continue;
        char interp[sizeof(linker)];
        ok = ph.p_filesz == sizeof(linker) && ph.p_offset <= (Elf64_Off)st.st_size &&
             sizeof(interp) <= (Elf64_Off)st.st_size - ph.p_offset &&
             pread(fd, interp, sizeof(interp), ph.p_offset) == sizeof(interp) &&
             !memcmp(interp, linker, sizeof(interp));
        break;
    }
done:
    close(fd);
    return ok;
}

// Bound stack usage even for an unexpected caller. Wine uses only a few args.
#define MAX_ARGS 4096
static bool linker_args(char *out[MAX_ARGS + 2], char *const argv[]) {
    out[0] = (char *)linker;
    for (unsigned i = 0; i < MAX_ARGS; ++i) {
        out[i + 1] = argv[i];
        if (!argv[i]) return true;
    }
    return false;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    int result = (int)syscall(SYS_execve, path, argv, envp);
    int error = errno;
    if (error == EACCES && can_link(path, argv)) {
        char *args[MAX_ARGS + 2];
        if (linker_args(args, argv)) return (int)syscall(SYS_execve, linker, args, envp);
    }
    errno = error;
    return result;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    if (!spawn_next) return ENOSYS;
    int result = spawn_next(pid, path, actions, attr, argv, envp);
    // File actions may change pathname resolution; Wine's wineserver uses none.
    if (result == EACCES && !actions && can_link(path, argv)) {
        char *args[MAX_ARGS + 2];
        if (linker_args(args, argv)) return spawn_next(pid, linker, NULL, attr, args, envp);
    }
    return result;
}
