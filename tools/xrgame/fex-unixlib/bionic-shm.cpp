// SPDX-License-Identifier: MIT
// FEX's optional statistics use named mappings. Bionic has no POSIX shm_open;
// keep those files in the launcher's app-private TMPDIR instead of /dev/shm.
#include "bionic-shm.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static int stats_directory(const char* name) {
    if (!name || std::strncmp(name, "fex-", 4) != 0) { errno = EINVAL; return -1; }
    const char* digit = name + 4;
    if (*digit < '0' || *digit > '9') { errno = EINVAL; return -1; }
    while (*digit >= '0' && *digit <= '9') ++digit;
    if (std::strcmp(digit, "-stats") != 0) { errno = EINVAL; return -1; }
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || tmp[0] != '/') { errno = ENOENT; return -1; }
    int dir = open(tmp, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    struct stat st {};
    if (fstat(dir, &st) != 0 || st.st_uid != getuid()) {
        close(dir); errno = EACCES; return -1;
    }
    return dir;
}

extern "C" int xrgame_shm_open(const char* name, int flags, mode_t) {
    int dir = stats_directory(name);
    if (dir < 0) return -1;
    int fd = openat(dir, name, (flags & ~O_TRUNC) | O_CLOEXEC | O_NOFOLLOW, 0600);
    int error = errno;
    close(dir);
    if (fd < 0) { errno = error; return -1; }
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1) {
        close(fd); errno = EACCES; return -1;
    }
    if (fchmod(fd, 0600) != 0 || ((flags & O_TRUNC) && ftruncate(fd, 0) != 0)) {
        error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}

extern "C" int xrgame_shm_unlink(const char* name) {
    int dir = stats_directory(name);
    if (dir < 0) return -1;
    int result = unlinkat(dir, name, 0);
    int error = errno;
    close(dir);
    errno = error;
    return result;
}
