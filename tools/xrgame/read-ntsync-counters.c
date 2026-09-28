// SPDX-License-Identifier: GPL-3.0-or-later
// Read the 15 AtomicU64 counters of pinned ntsync-android 7ce6435 without stopping
// or writing the target. Resolve COUNTERS from the exact mapped ELF first; never
// reuse an address across process generations. This is not a coherent snapshot:
// timestamps bracket each read, and individual fields may advance independently.
// Usage: reader PID START_TIME_TICKS COUNTERS_ADDRESS SECONDS INTERVAL_MS
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static uint64_t generation(int pid) {
    char path[64], buffer[2048];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *line = fgets(buffer, sizeof(buffer), f);
    fclose(f);
    if (!line) return 0;
    char *p = strrchr(buffer, ')'), *state;
    if (!p) return 0;
    char *value = strtok_r(p + 2, " ", &state);
    for (int field = 3; value; ++field, value = strtok_r(NULL, " ", &state))
        if (field == 22) return strtoull(value, NULL, 10);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    int pid = atoi(argv[1]), seconds = atoi(argv[4]), interval = atoi(argv[5]);
    uint64_t expected = strtoull(argv[2], NULL, 10);
    uint64_t address = strtoull(argv[3], NULL, 0);
    if (pid <= 0 || !expected || !address || (address & 7) ||
        seconds < 1 || seconds > 120 || interval < 100 || interval > 2000) return 2;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open target counters"); return 3; }
    uint64_t start = now(), next = start;
    for (int sample = 0; sample <= seconds * 1000 / interval; ++sample) {
        if (generation(pid) != expected) { fputs("target generation changed\n", stderr); close(fd); return 4; }
        uint64_t counters[15], before = now();
        ssize_t got = pread(fd, counters, sizeof(counters), (off_t)address);
        uint64_t after = now();
        if (got != sizeof(counters)) { fprintf(stderr, "counter read: bytes=%zd errno=%d\n", got, errno); close(fd); return 5; }
        printf("{\"pid\":%d,\"startTicks\":%llu,\"beforeNs\":%llu,\"afterNs\":%llu,\"counters\":[",
            pid, (unsigned long long)expected, (unsigned long long)before, (unsigned long long)after);
        for (int i = 0; i < 15; ++i) printf("%s%llu", i ? "," : "", (unsigned long long)counters[i]);
        puts("]}"); fflush(stdout);
        if (sample == seconds * 1000 / interval) break;
        next += (uint64_t)interval * 1000000ULL;
        struct timespec deadline = {(time_t)(next / 1000000000ULL), (long)(next % 1000000000ULL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
    }
    close(fd);
    return ferror(stdout) ? 1 : 0;
}
