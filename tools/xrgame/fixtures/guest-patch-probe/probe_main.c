/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest patch fixture: prints the probe functions' results twice, one second apart.
 * XRGAME_GUEST_PATCH_SELFLOAD names the installer DLL to load first, for a native run
 * without the Steam client loader (which injects it on the device). No CRT. */
typedef __WCHAR_TYPE__ wchar;
__declspec(dllimport) void* GetStdHandle(unsigned long id);
__declspec(dllimport) int WriteFile(void* file, const void* data, unsigned long size, unsigned long* written, void* o);
__declspec(dllimport) void* LoadLibraryW(const wchar* name);
__declspec(dllimport) unsigned long GetEnvironmentVariableW(const wchar* name, wchar* value, unsigned long size);
__declspec(dllimport) void Sleep(unsigned long milliseconds);
__declspec(dllimport) void ExitProcess(unsigned code);

int probe_add(int a, int b);
int probe_counted(void);
int probe_read(void);
int probe_before(int a);
int probe_replace(int a);
int probe_constant(void);

static void out(const char* text) {
    unsigned long n = 0, written;
    while (text[n]) ++n;
    WriteFile(GetStdHandle((unsigned long)-11), text, n, &written, 0);
}
static void number(const char* label, int value) {
    char buffer[32];
    int n = 0, negative = value < 0;
    unsigned v = negative ? 0u - (unsigned)value : (unsigned)value;
    do buffer[n++] = (char)('0' + v % 10), v /= 10; while (v);
    out(label);
    if (negative) out("-");
    char digits[32];
    for (int i = 0; i < n; ++i) digits[i] = buffer[n - 1 - i];
    digits[n] = 0;
    out(digits);
}

void start(void) {
    wchar dll[260];
    if (GetEnvironmentVariableW(L"XRGAME_GUEST_PATCH_SELFLOAD", dll, 260) && !LoadLibraryW(dll)) {
        out("selfload failed\n");
        ExitProcess(2);
    }
    for (int round = 0; round < 2; ++round) {
        number("round=", round);
        number(" add=", probe_add(2, 3));
        number(" counted=", probe_counted());
        number(" read=", probe_read());
        number(" before=", probe_before(1));
        number(" replace=", probe_replace(3));
        number(" constant=", probe_constant());
        out("\n");
        if (!round) Sleep(1500);
    }
    ExitProcess(0);
}
