// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only XInput probe: run inside an existing prefix; never changes game settings.
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE*);
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR arguments, int show) {
    (void)instance; (void)previous; (void)show;
    int seconds = atoi(arguments);
    if (seconds < 1 || seconds > 60) seconds = 2;
    FILE* log = fopen("C:\\xrgame\\input-probe.log", "w");
    if (!log) return 2;
    HMODULE module = LoadLibraryW(L"xinput1_3.dll");
    get_state_fn get_state = module ? (get_state_fn)GetProcAddress(module, "XInputGetState") : NULL;
    if (!get_state) { fprintf(log, "load_error=%lu\n", GetLastError()); fclose(log); return 3; }
    fprintf(log, "wine_pid=%lu seconds=%d\n", GetCurrentProcessId(), seconds);
    ULONGLONG start = GetTickCount64();
    do {
        for (DWORD slot = 0; slot < 4; ++slot) {
            XINPUT_STATE state = {0};
            DWORD status = get_state(slot, &state);
            fprintf(log, "ms=%llu slot=%lu status=%lu packet=%lu buttons=%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
                (unsigned long long)(GetTickCount64() - start), slot, status, state.dwPacketNumber,
                state.Gamepad.wButtons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);
        }
        fflush(log);
        Sleep(100);
    } while (GetTickCount64() - start < (ULONGLONG)seconds * 1000);
    FreeLibrary(module);
    fclose(log);
    return 0;
}
