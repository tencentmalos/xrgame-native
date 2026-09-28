# Personal v2 P1/P2 — AYN validation, 2026-09-26

Status: **in progress, not full P1/P2 acceptance**. Swan and Linux dependency
migration remain deferred. This record complements the original WP1/WP3 records;
the earlier AYANEO observations are not evidence for this device.

## Artifact and device identity

The first probe APK was built from the local working tree on
`feature/malos/wp1-steam-install` (base `ca045ed3`). Packaging started with an empty
APK output directory. `tools/audit-apk` checked 121 native/assets entries with zero
errors. The installed base APK hash matched the build output.

| Item | Identity |
|---|---|
| APK SHA-256 | `c50d8ec019a665d73c369fcd4b9c1c23a8da871c10f9ab6dff7a346a3e56c1de` |
| Component manifest SHA-256 | `83c79f72bf6ab259941d0658109c73258644fe37cd93e200a3c2f5654237d54d` |
| exec probe Build ID | `b8e19d33f2fd978f0891e5b4a42ae19939e5c1d5` |
| Vulkan renderer Build ID | `015a42ec199bb7510bfb14f0833511121c16099c` |
| winlator Build ID | `e120a35c35b69960ed4fe4c8993045bddffe4c1b` |
| extras Build ID | `4fc1ac1eba1ec240ee5e392ebaa7752097219dbe` |
| evshim Build ID | `cdfa530fd80649ca85e5f8b31308737bb9744f38` |
| PulseAudio Build ID | `ad97886e4ccdf2f02bec63d21c9733da0bfefe45` |
| Device | AYN Thor, Android 13 / API 33, ARM64, 4096-byte pages |
| Build | `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys` |
| App target | SDK 36, `com.tencentmalos.xrgamenative` |

Private evidence is under the sibling evidence repository,
`personal/20260926/ayn/`. `p1-probe-artifacts.json` records all 28 packaged `.so`
Build IDs. Boot ID, device serial, UID, `/proc` identities and screenshots remain
there, not in the public repository. Failed and aborted attempts were retained.

## P1 execution matrix in the application domain

Debug-only `ExecProbeActivity` requires `android.permission.DUMP`; it accepts no
command/path extras. It launches only the packaged source-owned probe, bounds its
lifetime to 45 seconds and writes a fresh evidence directory. Release builds do
not build/package the probe. These are app-domain results, not `adb shell` results.

Run `099b0b26-f08e-48e3-9410-71b76928a7fb`: app PID 16356, probe PID 16455,
duration **108,567,240 ns**, domain `u:r:untrusted_app:s0:c117,c256,c512,c768`.

| Operation | Observation |
|---|---|
| APK executable | exec succeeds, expected child exit 37 |
| Private copied executable, direct | succeeds; argv including empty/space/Chinese values and environment retained |
| Private copied executable via linker64 | succeeds; argv0 becomes executable path, `/proc/self/exe` is linker64 |
| Private file mmap RX / RWX | both succeed |
| Anonymous RW → RX / RWX | both succeed; AArch64 probe returns 42 |
| Read-only evdev open | 11 nodes open successfully; no reads or grabs |
| hidraw | no nodes present; not an access-denial result |
| Global SELinux enforcing state | unavailable (errno 13); not inferred from domain |

This matrix does not currently require an additional exec/W^X shim on AYN.
It says nothing about Android 16/Swan policy or physical controller operation.

## Source-owned x64 fixture

`tools/xrgame/fixtures/windows-probe.c` and `build-windows-probe.sh` build a
console/GDI/D3D11 test with the pinned llvm-mingw toolchain. The app imported it
into a new custom-game container; existing account/game data was preserved.
The default graphics run also launches a console child with empty, spaced and
Chinese arguments and a sentinel environment value; child exit 37 is required.

| Variant | Process / measurement | Result |
|---|---|---|
| Initial clear-only D3D11 | Android PID 25170, Wine PID 356; 7203 Presents / 120062 ms | HRESULT 0, normal exit, but black screenshot; retained as failed visual validation |
| Console + clear-only | Android PID 27958, Wine PID 240, console PID 308; 7204 / 120072 ms | child arguments/env/exit correct; black visual result retained |
| GDI | Android PID 29124, Wine PID 240; 152884 ms including debugger pauses | square visible, two key events, exit 0; duration is not a performance sample |
| One-shot diagnostic readback | Android PID 2552, Wine PID 236; 7204 / 120060 ms | first pixel RGBA `[26,26,179,255]`; blue visible, exit 0 |
| Animated triangle, no readback | Wine PID 240; 2087 / 34793 ms, one key event | triangle and changing background visible; Esc exit 0, no Wine residuals |

The last triangle attempt omitted Android PID capture, so it needs a complete
identity replay. The one-shot pixel readback is an explicit fixture option, not
the display path. Clear-only black output remains a separate unresolved result;
adding a draw does not prove a renderer fix.

## Host/Guest debugger

Host LLDB attached to the verified app PID 16356, paused at epoch 1, inspected
the epoll/Looper/MessageQueue/ART stack, resumed and detached. Cleanup checks at
1 and 5 seconds confirmed the same live process generation and TracerPid 0.
Wine's changed process cmdline is still rejected by the native debugger's
package identity check; it was not bypassed. Many modules lack exact local
symbols, so this is not complete dual-stack symbolization.

Guest proxy attached to GDI Android PID 29124 / Wine PID 240, paused, returned
register packets and module information, then detached with ownership released,
proxy exit 0 and its ADB forward removed. **The first MCP session omitted
`byteOrder=little`; its numeric register interpretation is invalid.** Raw bytes
are retained, and a corrected replay is required. A previous attach attempt
rejected an already-exited process before staging a proxy. No step/breakpoint
acceptance is claimed by these sessions.

## P2 Hades II diagnosis and application integration

The installed game is Steam app 1145350. Direct launch reached the same missing
client problem seen on AYANEO. A separate validation folder, without replacing
game DLLs, reproduced the known offline GBE client + `Ship/Hades2.exe /c ../`
combination. Source client/extra DLLs and the fixed upstream loader are individually
hashed; the loader is **not** a completed local source build.

First run reached `Renderer creation failed` / `No available D3D12 adapter found`.
The container had installed DXVK only and retained Wine's builtin d3d12.dll.
After installing the pinned VKD3D pair and restarting the app, game PID 15004
(Wine PID 300) reached the menu and a new save slot's actual scene. WASD movement
and touchpad mouse clicks worked. The short intervening failed launch is retained;
its exact cause is not established from the available log.

The code now selects the combined DXVK/VKD3D runtime and sets both environments.
The Hades profile stages the catalog-pinned client in the prefix, checks archive
hashes, repairs changed cached binaries, rejects unexpected archive members and
keeps user-selected tools on the direct-launch path. Client networking, overlay
injection and automatic DLC unlocking are disabled.

The integrated APK is `02562961641d7f32d359e974cb7eb7d8dbcdf633d66bbadfb645410b5e4a60fa`,
with catalog `c36d9169c1e5c39a34d6a7577b156b53eab7af3ef2735d680f4050dc50ef56f5`.
Its native Build IDs are unchanged from the first probe APK; the catalog adds the
explicitly mixed-source validation client archive
`5ad0cd0f91a0afe0c4b3279b3640b59bab5291bbbeb3025d762b41a3d35c22b7`.
The APK audit checked 122 entries with zero errors. Steam client archive/profile
unit tests passed. Source-built client DLLs are at GBE `7a319f0`; the loader stays
pinned to upstream release `2026_09_16_2` (`8479ddbc`). This is not a full-source
loader claim or public release catalog.

On app PID 20040, integrated launches reached Hades II PIDs 21133, 21744, 22327 and
26767. The first two reached the menu. The third reached a scene, crossed the first
door and produced real save files; captures of the same process span **811.481 s**
(including menus, not an uninterrupted gameplay measurement). All four normal
Alt+F4 exits left only the Android app process for its UID; no Wine processes
remained. Process start ticks and boot identity are recorded per cycle.

The new test slot generated `Profile1.sav` (21,022 bytes at the first checkpoint
capture). After exit, the fourth launch showed the retained slot (The Crossroads,
Nights 1) and loaded that saved scene. Its same-generation captures span **202.312 s**.
This verifies local save/reload for this build. The unlaunched `run4-start` capture
is retained separately and is not counted as a game run. Update preservation is
being tested with the next instrumented APK.

A 25-second scrcpy capture has H.264 1280×720 video (25.003211 s) and stereo
48 kHz Opus (24.732187 s). Decoded audio has a nonzero signal (peak −35.2 dBFS).
This verifies captured device output, not physical speaker audibility. The capture
session and recorder were stopped normally. Before application integration, game
PID 15004 was observed in the scene across **632.045 s**, including a brief pause
menu and movement checks. This is a runtime observation, not a frame-time benchmark.
It used the first probe APK plus the isolated client trial, so it is not silently
attributed to the later integrated build.

## Remaining acceptance work

- Complete a same-build ten-minute scene observation and application-update save
  preservation. Three clean integrated launch/exit cycles and local save/reload
  now pass. Physical controller and physical speaker assessment remain open.
- Correct little-endian Guest debugger replay, symbols, breakpoints and bounded
  stepping; native Wine process identity support remains a tool limitation.
- P2 download interruption/resume, validation, existing-directory import and
  exact app/depot/manifest SHA cross-check with DepotDownloader remain separate.
- P3 measurement and safe direct AHB sampling remain open; current successful
  scene uses the AHB GPU copy baseline, with no CPU readback.


## Continuation status, 2026-09-27

The remaining-work list above describes the initial checkpoint. Subsequent
same-build scene observations, update/save preservation, D3D11 clear-only fix,
sampling lifecycle tests and their limits are recorded in
[the P3/debugger continuation](personal-p3-debuggers-20260927.md).
Small/large Steam downloads, interruption/resume, full manifest checks, corruption
repair, directory re-recognition and shared-runtime DepotDownloader comparison
are recorded in [P2 download continuation](personal-p2-downloads-20260927.md).
Physical controller/speaker assessment, an independent paid-game DepotDownloader
copy and reliable guest breakpoint resume remain open.
