// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only, bounded Android /proc sampler. Run as the app UID for its Wine tasks.
// Usage: sample-process-load SECONDS INTERVAL_MS PID [PID ...]
// T blocks carry stat (including start time), schedstat and wchan; F blocks carry
// system counters. S timestamps use CLOCK_MONOTONIC. No process control is used.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void dump(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { printf("error=%d\n", errno); return; }
    char buffer[8192]; ssize_t count;
    char last = '\n';
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, (size_t)count, stdout);
        last = buffer[count - 1];
    }
    if (last != '\n') putchar('\n');
    close(fd);
}

static void field(const char *path) { printf("F %s\n", path); dump(path); }

static void tasks(int pid) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *dir = opendir(path);
    if (!dir) { printf("missing_pid=%d error=%d\n", pid, errno); return; }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long tid = strtol(entry->d_name, &end, 10);
        if (*end || tid <= 0) continue;
        printf("T %d %ld\n", pid, tid);
        const char *files[] = {"stat", "schedstat", "wchan"};
        for (size_t i = 0; i < 3; ++i) {
            snprintf(path, sizeof(path), "/proc/%d/task/%ld/%s", pid, tid, files[i]);
            dump(path);
        }
    }
    closedir(dir);
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 11) return 2;
    int seconds = atoi(argv[1]), interval = atoi(argv[2]);
    if (seconds < 1 || seconds > 60 || interval < 250 || interval > 2000) return 2;
    for (int i = 3; i < argc; ++i) if (atoi(argv[i]) <= 0) return 2;
    uint64_t start = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_PROCESS_CPUTIME_ID);
    uint64_t next = start, end = start + (uint64_t)seconds * 1000000000ULL;
    field("/proc/sys/kernel/random/boot_id");
    do {
        printf("S %llu\n", (unsigned long long)ns(CLOCK_MONOTONIC));
        for (int i = 3; i < argc; ++i) tasks(atoi(argv[i]));
        field("/proc/stat");
        field("/proc/pressure/cpu");
        field("/proc/pressure/memory");
        field("/sys/class/kgsl/kgsl-3d0/gpubusy");
        field("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
        field("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq");
        const int policies[] = {0, 3, 7}; // Current AYN topology, not an ABI claim.
        for (size_t i = 0; i < 3; ++i) {
            char path[256];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", policies[i]);
            field(path);
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", policies[i]);
            field(path);
        }
        printf("E %llu\n", (unsigned long long)ns(CLOCK_MONOTONIC));
        fflush(stdout);
        next += (uint64_t)interval * 1000000ULL;
        struct timespec deadline = {(time_t)(next / 1000000000ULL), (long)(next % 1000000000ULL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
    } while (ns(CLOCK_MONOTONIC) < end);
    printf("sampler_wall_ns=%llu sampler_cpu_ns=%llu\n",
        (unsigned long long)(ns(CLOCK_MONOTONIC) - start),
        (unsigned long long)(ns(CLOCK_PROCESS_CPUTIME_ID) - cpu));
    return ferror(stdout) ? 1 : 0;
}
