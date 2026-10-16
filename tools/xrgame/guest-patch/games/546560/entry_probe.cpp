// SPDX-License-Identifier: GPL-3.0-or-later
// Half-Life: Alyx (546560) chain probes: count the entry of hlvr.exe and the DllMain calls
// of client.dll (loaded later by the engine), then run the originals unchanged.

namespace {
constexpr xrg_u64 kCalls = 1; // counter: calls, last = argument of interest
constexpr xrg_u64 kEvent = 1; // log tag
} // namespace

extern "C" {
#ifdef XRG_ALYX_EXE
// hlvr.exe AddressOfEntryPoint (the CRT entry): Windows passes the PEB in rcx.
int patched_exe_entry(void* peb) {
    xrg_sdk_counter(kCalls, 1);
    xrg_sdk_log(kEvent, static_cast<xrg_i64>(xrg_sdk_clock_ns() / 1000000));
    return original_exe_entry(peb);
}
#else
// client.dll AddressOfEntryPoint (_DllMainCRTStartup): reason 1 attach, 2/3 threads, 0 detach.
int patched_client_dllmain(void* module, unsigned reason, void* reserved) {
    xrg_sdk_counter(kCalls, reason);
    if (reason == 0 || reason == 1) xrg_sdk_log(kEvent, reason);
    return original_client_dllmain(module, reason, reserved);
}
#endif
}
