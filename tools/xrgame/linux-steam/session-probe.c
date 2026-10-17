/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Public SteamClient020 / SteamUser021 interface slots, as used by Proton.
 * A separate, bounded process: no login, auth ticket or account identifier is printed.
 * The expected SteamID is read from stdin, never a command line/log. */
static void *method(void *object, unsigned slot) { return (*(void ***)object)[slot]; }
int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') return 2;
    // An app-scoped pipe registers a fake game (PID 0) with Steam. Never run this
    // session-only diagnostic with the game identity, even if inherited by the caller.
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");
    alarm(12);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts("steampsp-session:library-unavailable"); return 3; }
    void *(*create)(const char *, int *) = dlsym(lib, "CreateInterface");
    if (!create) return 3;
    void *client = create("SteamClient020", NULL);
    if (!client) { puts("steampsp-session:interface-unavailable"); return 4; }
    int pipe = ((int (*)(void *))method(client, 0))(client);
    if (!pipe) { puts("steampsp-session:no-pipe"); return 5; }
    int user = ((int (*)(void *, int))method(client, 2))(client, pipe);
    int result = 6;
    if (user) {
        void *account = ((void *(*)(void *, int, int, const char *))method(client, 5))(client, user, pipe, "SteamUser021");
        if (account && ((bool (*)(void *))method(account, 1))(account)) {
            char expected[32] = {0};
            if (fgets(expected, sizeof(expected), stdin) && expected[0] != '\n') {
                unsigned long long actual = ((unsigned long long (*)(void *))method(account, 2))(account);
                result = actual == strtoull(expected, NULL, 10) ? 0 : 8;
            } else result = 0;
        } else result = 7;
        ((void (*)(void *, int, int))method(client, 4))(client, pipe, user);
    }
    ((bool (*)(void *, int))method(client, 1))(client, pipe);
    puts(result == 0 ? "steampsp-session:online" : result == 8 ? "steampsp-session:account-mismatch" :
         "steampsp-session:not-signed-in");
    return result;
}
