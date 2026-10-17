/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <gnu/libc-version.h>

static __thread int tls_value;
static void *thread(void *unused) { tls_value = 42; return (void *)(long)tls_value; }
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    if (argc > 1) { puts("exec-child=ok"); return 0; }
    printf("glibc=%s uid=%d pid=%d\n", gnu_get_libc_version(), getuid(), getpid());
    pthread_t tid; void *value = NULL;
    if (pthread_create(&tid, NULL, thread, NULL) || pthread_join(tid, &value) || (long)value != 42 || tls_value) return 2;
    puts("pthread-tls=ok");
    int fds[2]; char c;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) || write(fds[0], "x", 1) != 1 || read(fds[1], &c, 1) != 1 || c != 'x') return 3;
    close(fds[0]); close(fds[1]); puts("unix-ipc=ok");
    pid_t child = fork();
    if (child == 0) { execl("/probe", "/probe", "child", NULL); perror("exec"); _exit(4); }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 4;
    puts("result=pass");
    return 0;
}
