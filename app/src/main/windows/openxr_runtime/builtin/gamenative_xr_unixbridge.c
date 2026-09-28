




/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* Wine supplies the ARM64EC dispatch thunk and the i386 stdcall declarations. */
NTSTATUS WINAPI gnWineUnixCall(unsigned int code, void *args)
{
    return WINE_UNIX_CALL(code, args);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        return __wine_init_unix_call() == 0;
    return TRUE;
}
