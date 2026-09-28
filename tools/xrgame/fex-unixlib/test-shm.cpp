// SPDX-License-Identifier: MIT
#include "bionic-shm.h"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    char root[] = "/tmp/xrgame-shm-XXXXXX";
    assert(mkdtemp(root));
    assert(setenv("TMPDIR", root, 1) == 0);
    const char* name = "fex-123-stats";
    int fd = xrgame_shm_open(name, O_CREAT | O_TRUNC | O_RDWR, 0777);
    assert(fd >= 0);
    struct stat st {};
    assert(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(ftruncate(fd, 4096) == 0);
    auto* shared = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(shared != MAP_FAILED);
    shared[0] = 42;
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        int reader = xrgame_shm_open(name, O_RDWR, 0777);
        char value = 0;
        if (reader < 0 || pread(reader, &value, 1, 0) != 1 || value != 42) _exit(1);
        value = 43;
        if (pwrite(reader, &value, 1, 0) != 1) _exit(2);
        close(reader);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(shared[0] == 43);
    assert(xrgame_shm_unlink(name) == 0);
    assert(shared[0] == 43); // Unlink must preserve existing mappings.
    assert(xrgame_shm_open(name, O_RDWR, 0600) == -1 && errno == ENOENT);
    munmap(shared, 4096);
    close(fd);

    // Reject traversal and refuse to truncate a symlink's target.
    assert(xrgame_shm_open("fex-1-stats/../../victim", O_CREAT | O_RDWR, 0600) == -1);
    std::string victim = std::string(root) + "/victim";
    int target = open(victim.c_str(), O_CREAT | O_RDWR, 0600);
    assert(target >= 0 && write(target, "keep", 4) == 4);
    std::string link = std::string(root) + '/' + name;
    assert(symlink(victim.c_str(), link.c_str()) == 0);
    assert(xrgame_shm_open(name, O_CREAT | O_TRUNC | O_RDWR, 0600) == -1);
    assert(fstat(target, &st) == 0 && st.st_size == 4);
    assert(unlink(link.c_str()) == 0);
    assert(unlink(victim.c_str()) == 0);
    close(target);
    unsetenv("TMPDIR");
    assert(xrgame_shm_open(name, O_CREAT | O_RDWR, 0600) == -1 && errno == ENOENT);
    assert(rmdir(root) == 0);
}
