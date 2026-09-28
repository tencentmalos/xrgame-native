// SPDX-License-Identifier: GPL-3.0-or-later
// Build twice: -shared -DBOOTSTRAP_DLL, and -municode for the executable.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#ifdef BOOTSTRAP_DLL
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t value[32], tls[32] = {0};
        swprintf(value, 32, L"%lu", GetCurrentProcessId());
        GetEnvironmentVariableW(L"XRGAME_FIXTURE_TLS_PID", tls, 32);
        if (wcscmp(value, tls)) return FALSE;
        SetEnvironmentVariableW(L"XRGAME_FIXTURE_BOOTSTRAP_PID", value);
    }
    return TRUE;
}
#else
static void NTAPI tls_callback(void *module, DWORD reason, void *reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t value[32];
        swprintf(value, 32, L"%lu", GetCurrentProcessId());
        SetEnvironmentVariableW(L"XRGAME_FIXTURE_TLS_PID", value);
    }
}
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK restart_probe_tls = tls_callback;

static void record(const char *event, BOOL ok) {
    FILE *f = fopen("restart-probe.jsonl", "a");
    if (!f) ExitProcess(90);
    fprintf(f, "{\"event\":\"%s\",\"pid\":%lu,\"ok\":%s}\n", event, GetCurrentProcessId(), ok ? "true" : "false");
    fclose(f);
}

static BOOL spawn(wchar_t *exe, const wchar_t *argument, BOOL wait) {
    wchar_t cmd[2048];
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION child = {0};
    swprintf(cmd, 2048, L"\"%ls\" %ls", exe, argument);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child);
    if (ok) {
        if (wait) {
            DWORD code;
            ok = WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(child.hProcess, &code) && code == 0;
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t value[32] = {0}, own[32], exe[1024];
    swprintf(own, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"XRGAME_FIXTURE_BOOTSTRAP_PID", value, 32);
    BOOL bootstrapped = !wcscmp(own, value);
    if (argc > 1 && !wcscmp(argv[1], L"--unrelated")) {
        record("unrelated-not-bootstrapped", !bootstrapped);
        return bootstrapped ? 2 : 0;
    }
    record("bootstrap-before-entry", bootstrapped);
    if (!bootstrapped) return 3;
    if (argc > 1 && !wcscmp(argv[1], L"--failure")) {
        record("intentional-failure-no-restart", TRUE);
        return 17;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--normal")) {
        record("normal-exit-no-restart", TRUE);
        return 0;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--child")) {
        Sleep(2000); // The original process has already exited.
        HKEY key;
        DWORD pid = 0, bytes = sizeof(pid);
        BOOL ok = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &key) == ERROR_SUCCESS;
        if (ok) {
            ok = RegQueryValueExW(key, L"pid", NULL, NULL, (BYTE *)&pid, &bytes) == ERROR_SUCCESS;
            RegCloseKey(key);
        }
        HANDLE owner = ok ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        ok = owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
        if (owner) CloseHandle(owner);
        record("successor-client-owner-alive", ok);
        return ok ? 0 : 4;
    }
    GetModuleFileNameW(NULL, exe, 1024);
    wchar_t helper[1024];
    wcscpy(helper, exe);
    wcscpy(wcsrchr(helper, L'\\') + 1, L"restart-unrelated.exe");
    BOOL ok = spawn(helper, L"--unrelated", TRUE) && spawn(exe, L"--child", FALSE);
    record("explicit-self-restart", ok);
    return ok ? 1 : 5;
}
#endif
