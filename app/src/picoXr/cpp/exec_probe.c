// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void quote(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void read_value(const char *name, const char *path, bool link) {
    char data[PATH_MAX] = {0};
    int error = 0;
    ssize_t length;
    if (link) length = readlink(path, data, sizeof(data) - 1);
    else {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) length = -1;
        else { length = read(fd, data, sizeof(data) - 1); error = errno; close(fd); errno = error; }
    }
    if (length < 0) error = errno;
    else { error = 0; while (length && (data[length-1] == '\n' || data[length-1] == '\0')) --length; data[length] = 0; }
    printf("{\"event\":\"identity\",\"pid\":%d,\"field\":", getpid()); quote(name);
    printf(",\"value\":"); quote(data); printf(",\"errno\":%d}\n", error);
}

static void identity(void) {
    printf("{\"event\":\"process\",\"pid\":%d,\"ppid\":%d,\"uid\":%d,\"pageSize\":%ld}\n",
           getpid(), getppid(), getuid(), sysconf(_SC_PAGESIZE));
    read_value("selinuxDomain", "/proc/self/attr/current", false);
    read_value("selinuxEnforce", "/sys/fs/selinux/enforce", false);
    read_value("selfExe", "/proc/self/exe", true);
}

static bool copy_file(const char *from, const char *to) {
    int input = open(from, O_RDONLY | O_CLOEXEC);
    if (input < 0) return false;
    int output = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (output < 0) { close(input); return false; }
    char buffer[16384];
    ssize_t length;
    bool ok = true;
    while ((length = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t done = 0;
        while (done < length) {
            ssize_t count = write(output, buffer + done, length - done);
            if (count <= 0) { ok = false; break; }
            done += count;
        }
        if (!ok) break;
    }
    if (length < 0) ok = false;
    close(input); close(output);
    return ok;
}

static pid_t bounded_fork(void) {
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(125);
    }
    return child;
}

static void collect(const char *name, pid_t child, uint64_t start) {
    int status = 0;
    bool timeout = false;
    if (child < 0) {
        printf("{\"event\":\"forkFailure\",\"errno\":%d}\n", errno);
        return;
    }
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) {
            printf("{\"event\":\"waitFailure\",\"errno\":%d}\n", errno);
            return;
        }
        if (now_ns() - start > 5000000000ULL) {
            timeout = true; kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        struct timespec delay = {.tv_nsec = 10000000}; nanosleep(&delay, NULL);
    }
    printf("{\"event\":\"result\",\"case\":"); quote(name);
    printf(",\"pid\":%d,\"exitCode\":%d,\"signal\":%d,\"timeout\":%s,\"durationNs\":%llu}\n",
           child, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0, timeout ? "true" : "false",
           (unsigned long long)(now_ns() - start));
}

static void exec_case(const char *name, const char *path, bool linker) {
    uint64_t start = now_ns();
    pid_t child = bounded_fork();
    if (child == 0) {
        char *env[] = {"XRPROBE_ENV=space 空值", "PATH=/system/bin", NULL};
        char *direct[] = {"xrgame-custom-argv0", "--child", "", "a b", "中文", NULL};
        char *indirect[] = {"xrgame-custom-argv0", (char *)path, "--child", "", "a b", "中文", NULL};
        execve(linker ? "/system/bin/linker64" : path, linker ? indirect : direct, env);
        int error = errno;
        printf("{\"event\":\"execFailure\",\"case\":"); quote(name);
        printf(",\"errno\":%d}\n", error);
        _exit(126);
    }
    collect(name, child, start);
}

static void wine_exec_case(const char *name, const char *path, int api, int expected_error) {
    uint64_t start = now_ns();
    pid_t child = bounded_fork();
    if (child == 0) {
        char *args[] = {(char *)path, "--child", "", "a b", "中文", NULL};
        setenv("XRPROBE_ENV", "space 空值", 1);
        setenv("XRPROBE_ARGV0", path, 1);
        int error;
        if (api == 2) {
            pid_t spawned;
            error = posix_spawn(&spawned, path, NULL, NULL, args, environ);
            if (!error) {
                int status;
                while (waitpid(spawned, &status, 0) < 0) if (errno != EINTR) _exit(124);
                // Bionic may report exec failure through the child's status,
                // which POSIX explicitly permits, instead of spawn's return.
                if (expected_error && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    printf("{\"event\":\"spawnExecFailure\",\"case\":"); quote(name);
                    printf(",\"exitCode\":127}\n");
                    _exit(0);
                }
                _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 123);
            }
        } else {
            if (api == 1) execv(path, args);
            else execve(path, args, environ);
            error = errno;
        }
        printf("{\"event\":\"execFailure\",\"case\":"); quote(name);
        printf(",\"errno\":%d,\"expectedErrno\":%d}\n", error, expected_error);
        _exit(expected_error && (error == expected_error ||
              (expected_error == ENOEXEC && error == EACCES)) ? 0 : 126);
    }
    collect(name, child, start);
}

static void memory_case(const char *name, const char *path, bool file, bool rwx) {
    uint64_t start = now_ns();
    pid_t child = bounded_fork();
    if (child == 0) {
        long size = sysconf(_SC_PAGESIZE);
        int fd = file ? open(path, O_RDONLY | O_CLOEXEC) : -1;
        int prot = file ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE;
        if (rwx) prot |= PROT_EXEC | PROT_WRITE;
        void *memory = mmap(NULL, size, prot, MAP_PRIVATE | (file ? 0 : MAP_ANONYMOUS), fd, 0);
        int error = memory == MAP_FAILED ? errno : 0;
        if (fd >= 0) close(fd);
        int result = -1;
        if (!error && !file) {
            const uint32_t code[] = {0x52800540, 0xd65f03c0}; // mov w0, #42; ret (AArch64)
            memcpy(memory, code, sizeof(code));
            __builtin___clear_cache(memory, (char *)memory + sizeof(code));
            if (!rwx && mprotect(memory, size, PROT_READ | PROT_EXEC) != 0) error = errno;
            if (!error) result = ((int (*)(void))memory)();
        }
        printf("{\"event\":\"memory\",\"case\":"); quote(name);
        printf(",\"errno\":%d,\"executedReturn\":%d}\n", error, result);
        if (memory != MAP_FAILED) munmap(memory, size);
        _exit(error ? 1 : (!file && result != 42 ? 2 : 0));
    }
    collect(name, child, start);
}

static void access_nodes(const char *directory, const char *prefix) {
    DIR *dir = opendir(directory);
    if (!dir) {
        int error = errno;
        printf("{\"event\":\"deviceDirectory\",\"path\":"); quote(directory);
        printf(",\"errno\":%d}\n", error); return;
    }
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(dir)) && count < 64) {
        if (strncmp(entry->d_name, prefix, strlen(prefix))) continue;
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (lstat(path, &st) || !S_ISCHR(st.st_mode)) continue;
        ++count;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        int error = fd < 0 ? errno : 0;
        if (fd >= 0) close(fd);
        printf("{\"event\":\"deviceOpen\",\"path\":"); quote(path);
        printf(",\"errno\":%d}\n", error);
    }
    closedir(dir);
    printf("{\"event\":\"deviceCount\",\"prefix\":"); quote(prefix);
    printf(",\"count\":%u}\n", count);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    identity();
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        const char *env = getenv("XRPROBE_ENV");
        bool ok = argc == 5 && !strcmp(argv[2], "") && !strcmp(argv[3], "a b") &&
            !strcmp(argv[4], "中文") && env && !strcmp(env, "space 空值");
        const char *expected_argv0 = getenv("XRPROBE_ARGV0");
        if (expected_argv0 && strcmp(argv[0], expected_argv0)) ok = false;
        printf("{\"event\":\"child\",\"argv0\":"); quote(argv[0]);
        printf(",\"argumentsAndEnvironmentMatch\":%s}\n", ok ? "true" : "false");
        return ok ? 37 : 38;
    }
    if (argc != 2 || argv[1][0] != '/') return 2;
    char private_exe[PATH_MAX];
    if (snprintf(private_exe, sizeof(private_exe), "%s/probe-private", argv[1]) >= (int)sizeof(private_exe)) return 2;
    if (!copy_file(argv[0], private_exe)) { printf("{\"event\":\"copyFailure\",\"errno\":%d}\n", errno); return 3; }
    exec_case("packaged_direct", argv[0], false);
    exec_case("private_direct", private_exe, false);
    exec_case("private_linker", private_exe, true);
    wine_exec_case("wine_execve", private_exe, 0, 0);
    wine_exec_case("wine_execv", private_exe, 1, 0);
    wine_exec_case("wine_spawn", private_exe, 2, 0);
    char missing[PATH_MAX], text_file[PATH_MAX];
    if (snprintf(missing, sizeof(missing), "%s/missing", argv[1]) >= (int)sizeof(missing) ||
        snprintf(text_file, sizeof(text_file), "%s/not-elf", argv[1]) >= (int)sizeof(text_file)) return 2;
    int text_fd = open(text_file, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0700);
    if (text_fd < 0) return 3;
    const char text[] = "not an ELF file\n";
    if (write(text_fd, text, sizeof(text)) != sizeof(text)) { close(text_fd); return 3; }
    close(text_fd);
    wine_exec_case("missing_exec", missing, 0, ENOENT);
    wine_exec_case("missing_spawn", missing, 2, ENOENT);
    wine_exec_case("non_elf_exec", text_file, 0, ENOEXEC);
    wine_exec_case("non_elf_spawn", text_file, 2, ENOEXEC);
    chmod(private_exe, 0600);
    wine_exec_case("non_executable", private_exe, 0, EACCES);
    chmod(private_exe, 0700);
    memory_case("private_file_rx", private_exe, true, false);
    memory_case("private_file_rwx", private_exe, true, true);
    memory_case("anonymous_rw_to_rx", private_exe, false, false);
    memory_case("anonymous_rwx", private_exe, false, true);
    access_nodes("/dev/input", "event");
    access_nodes("/dev", "hidraw");
    printf("{\"event\":\"complete\",\"pid\":%d}\n", getpid());
    return 0;
}
