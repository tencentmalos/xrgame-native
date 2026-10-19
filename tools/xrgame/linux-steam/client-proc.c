/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Use DroidDeck's narrow Steam/PRoot status fix without its game/GPU/input hooks.
 * Unchanged setuid/setgid calls succeed, refused hard links become exclusive copies and a refused
 * robust-list query gets a local head (Android 16 seccomp). The web helper can run with fewer
 * processes, and GPU drawing adds one opt-in environment switch for it. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
 * Android 16's app seccomp policy answers setuid/setgid with ENOSYS even when the ID does not
 * change. Xephyr drops to its own IDs before running xkbcomp and exits that child on failure,
 * so the keymap never compiles. A call that keeps every ID as it is has nothing to do.
 */
int setuid(uid_t uid) {
    uid_t real, effective, saved;
    if (getresuid(&real, &effective, &saved) == 0 && uid == real && uid == effective && uid == saved) return 0;
    int (*next)(uid_t) = dlsym(RTLD_NEXT, "setuid");
    return next(uid);
}
int setgid(gid_t gid) {
    gid_t real, effective, saved;
    if (getresgid(&real, &effective, &saved) == 0 && gid == real && gid == effective && gid == saved) return 0;
    int (*next)(gid_t) = dlsym(RTLD_NEXT, "setgid");
    return next(gid);
}
/*
 * Android 16 also refuses hard links in app data (EPERM). Xephyr creates its lock file with
 * link(); an exclusive copy of a regular file keeps the lock's create-once meaning.
 */
static int copy_instead_of_link(int olddir, const char *oldpath, int newdir, const char *newpath) {
    int in = openat(olddir, oldpath, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    struct stat info;
    if (fstat(in, &info) != 0 || !S_ISREG(info.st_mode)) { close(in); errno = EPERM; return -1; }
    int out = openat(newdir, newpath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, info.st_mode & 07777);
    if (out < 0) { int error = errno; close(in); errno = error; return -1; }
    char buffer[65536];
    ssize_t count;
    while ((count = read(in, buffer, sizeof buffer)) > 0)
        if (write(out, buffer, (size_t)count) != count) { count = -1; break; }
    int error = errno;
    close(in); close(out);
    if (count < 0) { unlinkat(newdir, newpath, 0); errno = error; return -1; }
    return 0;
}
int link(const char *oldpath, const char *newpath) {
    int (*next)(const char *, const char *) = dlsym(RTLD_NEXT, "link");
    int saved = errno, result = next(oldpath, newpath);
    if (result == 0 || errno != EPERM) return result;
    result = copy_instead_of_link(AT_FDCWD, oldpath, AT_FDCWD, newpath);
    if (result == 0) errno = saved;
    return result;
}
int linkat(int olddir, const char *oldpath, int newdir, const char *newpath, int flags) {
    int (*next)(int, const char *, int, const char *, int) = dlsym(RTLD_NEXT, "linkat");
    int saved = errno, result = next(olddir, oldpath, newdir, newpath, flags);
    if (result == 0 || errno != EPERM || (flags & AT_EMPTY_PATH)) return result;
    result = copy_instead_of_link(olddir, oldpath, newdir, newpath);
    if (result == 0) errno = saved;
    return result;
}
/*
 * Android 16's app seccomp policy also answers get_robust_list/set_robust_list with ENOSYS, so
 * glibc never registers a robust futex list and Steam's IPC mutexes stop with "futex robust_list
 * not initialized by pthreads". Steam asks through libc syscall(); give each thread an empty list
 * head laid out as glibc's. The kernel walks no list either way, as on a kernel without robust
 * futexes: a process that dies holding one of Steam's IPC locks is not recovered by the kernel.
 */
#include <linux/futex.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/syscall.h>
// As in glibc's struct pthread: robust_prev sits right before the head, and mutexes are queued
// with the word before an entry's next pointer as its prev, the head's included.
static __thread struct { void *prev; struct robust_list_head head; } robust;
_Static_assert(offsetof(__typeof__(robust), head) == sizeof(void *), "robust_prev must precede the head");
long syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    long a0 = va_arg(args, long), a1 = va_arg(args, long), a2 = va_arg(args, long);
    long a3 = va_arg(args, long), a4 = va_arg(args, long), a5 = va_arg(args, long);
    va_end(args);
    long (*next)(long, ...) = dlsym(RTLD_NEXT, "syscall");
    int saved = errno;
    long result = next(number, a0, a1, a2, a3, a4, a5);
    if (result != -1 || errno != ENOSYS || number != SYS_get_robust_list ||
        (a0 != 0 && a0 != (long)next(SYS_gettid)) || !a1 || !a2) return result;
    if (!robust.head.list.next) {
        robust.head.list.next = &robust.head.list;
        robust.prev = &robust.head;
        robust.head.futex_offset = (long)offsetof(pthread_mutex_t, __data.__lock) -
                                   (long)offsetof(pthread_mutex_t, __data.__list.__next);
    }
    *(struct robust_list_head **)a1 = &robust.head;
    *(size_t *)a2 = sizeof robust.head;
    static int reported;
    if (!__atomic_exchange_n(&reported, 1, __ATOMIC_RELAXED)) {
        char text[160];
        snprintf(text, sizeof text, "SteamPSP: %s robust_list query answered locally (seccomp ENOSYS)\n", program_invocation_short_name);
        (void)!write(2, text, strlen(text));
    }
    errno = saved;
    return 0;
}
/*
 * steamclient.so's syscall() binds past the preloaded definition (as RTLD_DEEPBIND would), so
 * point every loaded object's syscall() PLT slot here after each dlopen.
 */
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
static int bind_syscall(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libsteampsp-client-compat")) return 0;
    const ElfW(Dyn) *dynamic = NULL;
    ElfW(Addr) relro = 0, relro_end = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[i];
        if (header->p_type == PT_DYNAMIC) dynamic = (const ElfW(Dyn) *)(info->dlpi_addr + header->p_vaddr);
        if (header->p_type == PT_GNU_RELRO) { relro = info->dlpi_addr + header->p_vaddr; relro_end = relro + header->p_memsz; }
    }
    if (!dynamic) return 0;
    ElfW(Addr) symbols = 0, strings = 0, relocations = 0, bytes = 0;
    for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; entry++) {
        if (entry->d_tag == DT_SYMTAB) symbols = entry->d_un.d_ptr;
        else if (entry->d_tag == DT_STRTAB) strings = entry->d_un.d_ptr;
        else if (entry->d_tag == DT_JMPREL) relocations = entry->d_un.d_ptr;
        else if (entry->d_tag == DT_PLTRELSZ) bytes = entry->d_un.d_val;
    }
    if (!symbols || !strings || !relocations) return 0;
    // ld.so relocates these entries in place; an object it did not relocate has small offsets.
    if (symbols < info->dlpi_addr) symbols += info->dlpi_addr;
    if (strings < info->dlpi_addr) strings += info->dlpi_addr;
    if (relocations < info->dlpi_addr) relocations += info->dlpi_addr;
    const ElfW(Rela) *slot = (const ElfW(Rela) *)relocations;
    for (size_t i = 0; i < bytes / sizeof *slot; i++) {
        if (ELF64_R_TYPE(slot[i].r_info) != R_AARCH64_JUMP_SLOT) continue;
        const ElfW(Sym) *symbol = (const ElfW(Sym) *)symbols + ELF64_R_SYM(slot[i].r_info);
        if (strcmp((const char *)strings + symbol->st_name, "syscall") != 0) continue;
        void **target = (void **)(info->dlpi_addr + slot[i].r_offset);
        if (*target == (void *)syscall) continue;
        long page = sysconf(_SC_PAGESIZE);
        void *start = (void *)((ElfW(Addr))target & ~(ElfW(Addr))(page - 1));
        int protected = (ElfW(Addr))target >= relro && (ElfW(Addr))target < relro_end;
        if (protected && mprotect(start, (size_t)page, PROT_READ | PROT_WRITE) != 0) continue;
        *target = (void *)syscall;
        if (protected) mprotect(start, (size_t)page, PROT_READ);
    }
    return 0;
}
void *dlopen(const char *file, int mode) {
    void *(*next)(const char *, int) = dlsym(RTLD_NEXT, "dlopen");
    void *handle = next(file, mode);
    if (handle) dl_iterate_phdr(bind_syscall, NULL);
    return handle;
}
#if defined(__aarch64__)
__attribute__((constructor)) static void bind_loaded_syscall(void) { dl_iterate_phdr(bind_syscall, NULL); }
#endif
/*
 * Diagnostics only (XRGAME_STEAM_CRASH_TRACE): Steam's breakpad cannot ptrace a process PRoot
 * already traces, so its crashes leave no minidump. Report the first fatal signal's registers and
 * the mappings they fall in, then hand the signal to whatever handler the program installed.
 */
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
static int crash_trace;
static struct sigaction crash_next[3];
static volatile int crash_seen[3];
static int crash_slot(int sig) { return sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : sig == SIGILL ? 2 : -1; }
static void crash_print(const char *text) { (void)!write(2, text, strlen(text)); }
static void crash_maps(unsigned long pc, unsigned long lr, unsigned long addr) {
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buffer[8192], line[512];
    size_t used = 0;
    ssize_t count;
    while ((count = read(fd, buffer, sizeof buffer)) > 0) {
        for (ssize_t i = 0; i < count; i++) {
            if (buffer[i] != '\n' && used < sizeof line - 2) { line[used++] = buffer[i]; continue; }
            line[used] = 0;
            unsigned long start = strtoul(line, NULL, 16), end = strtoul(strchr(line, '-') ? strchr(line, '-') + 1 : line, NULL, 16);
            if ((pc >= start && pc < end) || (lr >= start && lr < end) || (addr >= start && addr < end)) {
                line[used] = '\n'; line[used + 1] = 0;
                crash_print("SteamPSP crash map: "); crash_print(line);
            }
            used = 0;
        }
    }
    close(fd);
}
static void crash_report(int sig, siginfo_t *info, void *context) {
    int slot = crash_slot(sig);
    unsigned long long pc = 0, lr = 0, sp = 0;
#ifdef __aarch64__
    ucontext_t *uc = context;
    pc = uc->uc_mcontext.pc; lr = uc->uc_mcontext.regs[30]; sp = uc->uc_mcontext.sp;
#else
    (void)context;
#endif
    char text[256];
    snprintf(text, sizeof text, "SteamPSP crash: %s pid=%d tid=%ld sig=%d code=%d addr=%p pc=%llx lr=%llx sp=%llx\n",
             program_invocation_short_name, getpid(), (long)syscall(SYS_gettid), sig, info->si_code, info->si_addr,
             pc, lr, sp);
    crash_print(text);
    crash_maps(pc, lr, (unsigned long)info->si_addr);
    // Give the signal back to the program's handler (or the default) and let the fault repeat.
    crash_seen[slot] = 1;
    int (*next)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    if (next(sig, &crash_next[slot], NULL) != 0) signal(sig, SIG_DFL);
}
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    int slot = crash_slot(sig);
    if (crash_trace && slot >= 0 && !crash_seen[slot]) {
        if (old) *old = crash_next[slot];
        if (act) crash_next[slot] = *act;
        return 0;
    }
    int (*next)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    return next(sig, act, old);
}
__attribute__((constructor)) static void crash_trace_start(void) {
    if (!getenv("XRGAME_STEAM_CRASH_TRACE")) return;
    int (*next)(int, const struct sigaction *, struct sigaction *) = dlsym(RTLD_NEXT, "sigaction");
    struct sigaction mine = {0};
    mine.sa_sigaction = crash_report;
    mine.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&mine.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL};
    for (int i = 0; i < 3; i++) next(signals[i], &mine, &crash_next[crash_slot(signals[i])]);
    crash_trace = 1;
}
/*
 * Fewer web helper processes (XRGAME_WEBHELPER_FEWER_PROCESSES): Android kills an app's child
 * processes beyond a device-wide limit (32 by default), and Steam with a running game is close to
 * it. Start the browser process again with no zygotes and the network and audio services inside
 * it, keeping the features Steam already sets (Chromium reads the last copy of a switch).
 */
#include <stdio.h>
static int webhelper_fold(const char *list, const char *name) {
    if (strcmp(list, "1") == 0) return 1;
    size_t length = strlen(name);
    for (const char *at = strstr(list, name); at; at = strstr(at + 1, name))
        if ((at == list || at[-1] == ',') && (at[length] == 0 || at[length] == ',')) return 1;
    return 0;
}
__attribute__((constructor)) static void webhelper_processes(int argc, char **argv) {
    // A comma list of zygote, network and audio; "1" folds all three.
    const char *fold = getenv("XRGAME_WEBHELPER_FEWER_PROCESSES");
    if (!fold || getenv("XRGAME_WEBHELPER_REEXEC") || strcmp(program_invocation_short_name, "steamwebhelper") != 0) return;
    for (int i = 1; i < argc; i++) if (argv[i] && strncmp(argv[i], "--type=", 7) == 0) return;
    int want_zygote = webhelper_fold(fold, "zygote"), want_network = webhelper_fold(fold, "network");
    int want_audio = webhelper_fold(fold, "audio");
    if (!want_zygote && !want_network && !want_audio) return;
    char **next = calloc((size_t)argc + 4, sizeof *next);
    if (!next) return;
    int count = 0, zygote = !want_zygote, enable = !want_network, disable = !want_audio;
    for (int i = 0; i < argc; i++) {
        char *merged = NULL;
        if (strcmp(argv[i], "--no-zygote") == 0) zygote = 1;
        if (want_network && strncmp(argv[i], "--enable-features=", 18) == 0 &&
            asprintf(&merged, "%s,NetworkServiceInProcess2", argv[i]) > 0) enable = 1;
        if (want_audio && strncmp(argv[i], "--disable-features=", 19) == 0 &&
            asprintf(&merged, "%s,AudioServiceOutOfProcess", argv[i]) > 0) disable = 1;
        next[count++] = merged ? merged : argv[i];
    }
    if (!zygote) next[count++] = "--no-zygote";
    if (!enable) next[count++] = "--enable-features=NetworkServiceInProcess2";
    if (!disable) next[count++] = "--disable-features=AudioServiceOutOfProcess";
    next[count] = NULL;
    setenv("XRGAME_WEBHELPER_REEXEC", "1", 1);
    execv("/proc/self/exe", next);
    unsetenv("XRGAME_WEBHELPER_REEXEC");  // Keep running with Steam's own arguments.
}
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
