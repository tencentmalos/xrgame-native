// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only, bounded diagnostic for ntsync-android 7ce6435 (Android arm64, v9).
// No ptrace, target writes, signal delivery, or target-side instrumentation.
// Usage: reader PID START_TICKS SHM_BASE SECONDS INTERVAL_MS TID [TID ...]
// Resolve SHM_BASE from this process's maps; verify the exact ntdll SHA first.
// Repeated matching reads reduce races; they do NOT constitute an atomic snapshot.
// Layout provenance: tencentmalos/ntsync-android, src/core.rs at the pin above.
// Times are occupancy samples, never API counts or measured wait durations.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { HEADER_BYTES = 88, SLOT_BYTES = 40, NODE_BYTES = 40,
       SLOT_COUNT = 16384, NODE_COUNT = 8192, MAX_THREADS = 8 };
typedef struct {
    uint64_t magic;
    uint32_t version, capacity, node_capacity, free_head, fresh, scan;
    uint64_t sweep;
    uint32_t owner_pid, owner_tid;
    unsigned char mutex[40];
} Header;
typedef struct {
    uint32_t state, generation, type, pid, waiter_head, pad;
    uint64_t word, pulse;
} Slot;
typedef struct {
    uint32_t seq, registered, obj, word, prev, next, waiter_next, pid;
    uint64_t wake;
} Node;
typedef struct { long nr; uint64_t a[6], sp, pc; } Syscall;
_Static_assert(sizeof(Header) == HEADER_BYTES, "pinned header");
_Static_assert(sizeof(Slot) == SLOT_BYTES, "pinned slot");
_Static_assert(sizeof(Node) == NODE_BYTES, "pinned node");

static uint64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t generation(int pid, int tid) {
    char path[80], buffer[4096], *save;
    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char *ok = fgets(buffer, sizeof(buffer), f);
    fclose(f);
    if (!ok) return 0;
    char *end = strrchr(buffer, ')');
    if (!end) return 0;
    char *p = strtok_r(end + 2, " ", &save);
    for (int n = 3; p; ++n, p = strtok_r(NULL, " ", &save))
        if (n == 22) return strtoull(p, NULL, 10);
    return 0;
}
static int syscall_at(int pid, int tid, Syscall *s) {
    char path[80], buffer[512];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/syscall", pid, tid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *ok = fgets(buffer, sizeof(buffer), f);
    fclose(f);
    if (!ok) return -1;
    memset(s, 0, sizeof(*s));
    if (!strncmp(buffer, "running", 7)) { s->nr = -1; return 0; }
    return sscanf(buffer, "%ld %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64
                  " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64,
                  &s->nr, &s->a[0], &s->a[1], &s->a[2], &s->a[3],
                  &s->a[4], &s->a[5], &s->sp, &s->pc) == 9 ? 0 : -1;
}
static int read_at(int fd, uint64_t addr, void *out, size_t size) {
    return pread(fd, out, size, (off_t)addr) == (ssize_t)size;
}
static int valid_header(const Header *h) {
    return h->magic == UINT64_C(0x6e7473796e635f75) && h->version == 9 &&
           h->capacity == SLOT_COUNT && h->node_capacity == NODE_COUNT &&
           h->fresh <= NODE_COUNT;
}
int main(int argc, char **argv) {
    if (argc < 7 || argc > 6 + MAX_THREADS) return 2;
    int pid = atoi(argv[1]), seconds = atoi(argv[4]), interval = atoi(argv[5]);
    uint64_t expected = strtoull(argv[2], NULL, 10), base = strtoull(argv[3], NULL, 0);
    // argv[6] is the first TID (argc includes the executable).
    int tids[MAX_THREADS], nthreads = argc - 6;
    uint64_t tid_generations[MAX_THREADS];
    if (pid <= 0 || !expected || !base || (base & 4095) || seconds < 1 ||
        seconds > 60 || interval < 5 || interval > 1000 || nthreads > MAX_THREADS) return 2;
    for (int i = 0; i < nthreads; ++i) {
        tids[i] = atoi(argv[6 + i]);
        if (tids[i] <= 0 || !(tid_generations[i] = generation(pid, tids[i]))) return 4;
    }
    if (generation(pid, pid) != expected) return 4;
    char path[80];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("target memory"); return 3; }
    Header h;
    if (!read_at(fd, base, &h, sizeof(h)) || !valid_header(&h)) return 5;
    Node *nodes = calloc(NODE_COUNT, sizeof(Node));
    if (!nodes) return 6;
    uint64_t nodes_base = base + HEADER_BYTES + SLOT_COUNT * SLOT_BYTES;
    uint64_t started = now(), next = started;
    struct timespec cpu_start, cpu_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    for (int sample = 0; sample < seconds * 1000 / interval; ++sample) {
        if (generation(pid, pid) != expected) return 4;
        for (int t = 0; t < nthreads; ++t) {
            if (generation(pid, tids[t]) != tid_generations[t]) return 4;
            Syscall s, after;
            uint64_t before = now();
            if (syscall_at(pid, tids[t], &s)) return 7;
            printf("{\"beforeNs\":%" PRIu64 ",\"tid\":%d,\"nr\":%ld", before, tids[t], s.nr);
            if (s.nr == 98) {
                printf(",\"futex\":\"0x%" PRIx64 "\",\"op\":%" PRIu64, s.a[0], s.a[1]);
                uint64_t offset = s.a[0] - nodes_base;
                if ((s.a[1] & 127) == 0 && offset < NODE_COUNT * NODE_BYTES && !(offset % NODE_BYTES)) {
                    uint32_t head = (uint32_t)(offset / NODE_BYTES);
                    Node n0, n1;
                    int ok = read_at(fd, s.a[0], &n0, sizeof(n0)) &&
                             read_at(fd, base, &h, sizeof(h)) && valid_header(&h) && head < h.fresh &&
                             read_at(fd, nodes_base, nodes, h.fresh * sizeof(Node));
                    printf(",\"head\":%u,\"objects\":[", head);
                    int emitted = 0;
                    if (ok) for (uint32_t j = 0; j < h.fresh; ++j) {
                        Node *n = &nodes[j];
                        if (n->word != head || n->pid != (uint32_t)pid || !n->registered || n->obj >= SLOT_COUNT) continue;
                        Slot a, b;
                        uint64_t addr = base + HEADER_BYTES + n->obj * SLOT_BYTES;
                        if (!read_at(fd, addr, &a, sizeof(a)) || !read_at(fd, addr, &b, sizeof(b)) ||
                            memcmp(&a, &b, sizeof(a)) || a.state != 1 || a.type < 1 || a.type > 3) { ok = 0; continue; }
                        printf("%s{\"slot\":%u,\"generation\":%u,\"type\":%u,\"creatorPid\":%u,\"word\":%" PRIu64 "}",
                               emitted++ ? "," : "", n->obj, a.generation, a.type, a.pid, a.word);
                    }
                    ok = ok && read_at(fd, s.a[0], &n1, sizeof(n1)) && !memcmp(&n0, &n1, sizeof(n0)) &&
                         n0.seq == s.a[2] && n0.pid == (uint32_t)pid &&
                         !syscall_at(pid, tids[t], &after) && !memcmp(&s, &after, sizeof(s));
                    printf("],\"stable\":%s", ok && emitted ? "true" : "false");
                }
            }
            printf(",\"afterNs\":%" PRIu64 "}\n", now());
        }
        fflush(stdout);
        next += (uint64_t)interval * 1000000;
        struct timespec deadline = {(time_t)(next / 1000000000), (long)(next % 1000000000)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
    }
    free(nodes);
    close(fd);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    int64_t cpu_ns = (int64_t)(cpu_end.tv_sec - cpu_start.tv_sec) * 1000000000 +
                     cpu_end.tv_nsec - cpu_start.tv_nsec;
    fprintf(stderr, "{\"pid\":%d,\"startTicks\":%" PRIu64 ",\"samples\":%d,"
                    "\"elapsedNs\":%" PRIu64 ",\"observerCpuNs\":%" PRId64 "}\n",
            pid, expected, seconds * 1000 / interval * nthreads, now() - started, cpu_ns);
    return ferror(stdout) ? 1 : 0;
}
