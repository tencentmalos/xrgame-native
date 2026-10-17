/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

/* Load-only probe. Does not log in, request auth tickets, or alter an account. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setbuf(stdout, NULL);
    printf("bionic-client-pid=%d\n", getpid());
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    const char *symbols[] = { "CreateInterface", "Steam_BGetCallback", "Steam_GetAPICallResult",
        "Steam_FreeLastCallback", "Steam_ReleaseThreadLocalMemory", "Steam_IsKnownInterface",
        "Steam_NotifyMissingInterface", "Steam_CreateSteamPipe", "Steam_ConnectToGlobalUser", "Steam_BLoggedOn" };
    for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        if (!dlsym(module, symbols[i])) { fprintf(stderr, "missing: %s\n", symbols[i]); return 4; }
    }
    puts("official-bionic-dlopen=pass exports=10 (no-session-test)");
    /* Let process teardown unload the library; no undocumented shutdown call. */
    return 0;
}
