# Personal v2 — display and debugger continuation, 2026-09-27

Status: **P1/P2/P3 remain in progress**. AYN Thor / Android 13 / 4 KiB;
Swan and Linux dependency migration remain deferred. No commits, pushes or
public releases were made. Raw evidence is outside the repository under
`personal/20260926/ayn/`, including failed and interrupted attempts.

## Runtime and save continuation

The P2 integrated APK's real Hades II saves were hashed before and after a
preserve-data update. All observed `.sav`, `.sjson` and active-profile hashes
matched. Evidence: `p2-cycles/pre-update-save-hashes.txt` and
`post-update-save-hashes.txt`. The following launch loaded The Crossroads.

The instrumented APK SHA is
`751d5f3ed322968250e8688e8ede5b5b868c5de47d9ef0721db64457226c6a39`,
catalog `c36d9169c1e5c39a34d6a7577b156b53eab7af3ef2735d680f4050dc50ef56f5`,
renderer Build ID `f60fd56204b385ad2372d2e849edc13dcd0c3905`.
App PID 4185 / Hades II PID 5328 remained the same generation across **612.662 s**
of visible Crossroads observations, with keyboard movement near the end.
Alt+F4 exited normally and no Wine processes remained. Verbose Wine logging was
enabled: this is a runtime observation, not a quiet performance benchmark.
Boot ID, complete device identity and all native Build IDs are in private records.

Two earlier launches on app PID 32700 ended in about two seconds, before D3D
initialization. A later app restart succeeded. The cause is still open; enabling
logging is not claimed as a fix.

## P3 instrumentation and synchronization

Opt-in `XRGAME_PRESENT_TRACE_FRAMES` correlates receive, GPU copy completion,
Present Complete and Idle using one generation/frame identity. It is bounded to
3600 frames and defaults off. The analyzer retains incomplete frames and checks
ordering; four unit cases passed. Native aggregates separate mutex acquisition
and fence waiting. These are host wall-clock timings, not GPU timestamps or
proof of physical scanout completion.

In `p3-copy-trace.log`, 1793 distinct frames were observed, 1772 complete and 21
incomplete, with no detected ordering error. For the complete correlated samples:
mean copy-call duration 6.492214 ms, mutex wait 4.842755 ms, fence wait 0.922017 ms;
p95 values 13.865, 12.4564 and 1.989218 ms respectively. This startup/menu/transition
capture included verbose Wine logging and cannot establish a performance gain.
All observed Present wait-fence IDs were zero.

`SyncExtension.awaitFence` previously held its monitor while busy-waiting, which
blocked `triggerFence` from acquiring that same monitor. `SyncFenceRegistry` now
uses wait/notify, validates all input IDs and retains object identity across XID
reuse. Four concurrency tests cover trigger, destruction/reuse, validation and
interruption. Destruction releases existing waiters, as specified by the
[XSync protocol](https://xorg.freedesktop.org/archive/X11R7.7/doc/xextproto/sync.html).

Debug-only `XRGAME_AHB_OWNERSHIP=foreign` enables the available foreign queue-family
ownership extension and brackets the AHB copy with ownership barriers. It defaults
off. APK `ab423de5d76c40d0ab121670778d71c98dfe9b5c1eb4e3b64c658aaed5ac6a19`,
same catalog, renderer Build ID `72f72eea17d99961c528e1128cf59b3ed2dd85b9`:
app PID 9112 / probe PID 10645 still produced a black clear-only D3D11 window.
The option was removed from the test container afterwards. This experiment did
not fix the clear-only case. The subsequent direct-sampling experiment is recorded below.

## Guest debugging

The corrected little-endian r7 session on GDI PID 12114 returned plausible x64
registers and module addresses. Z0 software breakpoint insertion was rejected;
an accepted step command did not produce an observed step stop. Cleanup released
MCP ownership, removed its forward and the proxy exited 0.

r8 added x86 software breakpoint insertion/removal, instruction-cache flushing,
original-byte retention and detach restoration. Its test exposed a separate Wine
RSP issue: plain `c` resumed only the temporary break-in thread, leaving the
fixture thread suspended. r9 restores all threads for default continue and uses
the stopped thread for default single-step. Explicit thread selection remains.

r9 on GDI PID 27469 reached the instrumented add call with RCX=1 and
RDX=0x12345678, but stopped at native PC `0x7fdffd049c`, instruction `BRK #0`,
as SIGILL. This is FEX's guest-trap conversion entry, not a verified x64 breakpoint
stop. The breakpoint was removed and MCP/proxy cleanup completed, but the fixture
remained stuck and required force-stop. This failed run is retained in
`debugger-r9/`; cleanup receipt alone is not counted as resumed application health.

The next revision passes this first-chance dispatcher trap to FEX, in addition
to the existing suspension trap, so FEX can reconstruct the guest exception.
Breakpoint/step acceptance remains open until that behavior is measured.

r10/r11 did reach x64 PC `0x1400013e0` with RCX=1, RDX=0x12345678 and the
fixture's expected guest stack. Original bytes `55 48 89 e5 48 8d 04 11` were
restored on removal. However, subsequent plain continue, locked step and unlocked
step did not complete the function or produce an observed step stop. r11's trace
shows suspend count 0 and resume count 1, and the breakpoint is second-chance.
Cleanup reported `guest_stop_timeout`; the proxy exited but the probe required
force-stop. Neither successful hit nor MCP `cleaned` is full D2 acceptance.
Software breakpoint insertion is therefore gated behind an explicit fixture-only
helper option. The default helper keeps the previously validated inspection path.

r11 also validates the Wine PID against the expected Android PID using
`ProcessWineUnixPid` before attaching. A mismatched attempt was rejected with
error 87. The correct Wine PID 240 mapped to Android PID 13116, start ticks
122618630. Its debugger binary SHA is
`baab8f94dc03880c7d41c61ac7c49c3729a4643fa92d0a307a866c0ca720dd10`.
The Python host-identity helper suite has 13 passing cases; this is separate from the on-device Wine PID guard test. Native child-process
stack capture remains blocked by the current tool transport: the app selector
rejects Wine's changed name, and the system-process path requires a patched
run-as absent on this device. No system run-as was replaced for this test.

## Source-built Steam loader

`build-steamclient-loader.sh` builds fixed GBE `7a319f0` with the pinned Linux
llvm-mingw compiler. It records source archive, compiler, recipe, optional local
patch and binary SHA. This removes the need for a downloaded loader in private
validation; it does not complete public distribution/source prerequisites.

Unpatched source loader SHA
`0e4808384ea64b2e9eef93a5d395fb7ed735bcef68a8bdca3c298b373f7beecd`
was packaged in APK `b8c4d4fed6e8ea6ed0961df4b596de4cb7baa5b567ce05abd65ad5b1ac9fbf88`,
catalog `55da97e50fc420271c9ec0d8929a278f8284e02850e4d3baa1b0ffeac3c6b9e2`.
App PID 21066's first Hades launch exited immediately after DLL injection; a
retry reached the menu as PID 23229 and exited cleanly. Debug loader logs retain
the failure. The loader previously waited on the initial thread, not the whole
process, and ignored remote-thread exit status. A local diagnostic/lifetime patch
is under test; it is not yet claimed to solve the intermittent startup failure.

The lifetime patch waits for the process rather than only its initial thread and
records resume/wait/exit results. APK
`8090ae73e259de51ab1a209cba29a66111f634aa37d6bd0cd6d9ac4b94c2c7a5`,
catalog `9845f3a6bff1531efcaf43e777a50b4bbf25022b4ed724844113b388e190ad34`,
renderer Build ID `72f72eea17d99961c528e1128cf59b3ed2dd85b9`, passed the
122-entry audit. Its first launch reached the menu as Hades PID 2354 / app
PID 32053. Loader wait completed at 33634 ms with process/thread exit 0 after
Alt+F4; no Wine processes remained. One successful run does not establish that
the intermittent failure is fixed.

A second cold app launch on the lifetime-patched APK reached the Hades menu as
app PID 18663 / Hades PID 20294, then exited normally with no Wine children.
Evidence: `p2-lifetime/launch2-{running,cleanup}.json` and `launch2-loader.log`.
Two successes do not establish the intermittent failure's root cause.

The clear-only fixture was also tried with `TU_DEBUG=sysmem`: app PID 21713,
Android fixture PID 22929 / Wine PID 236. D3D11 and shader creation returned 0,
6264 frames were presented over 104413 ms, and Esc produced result 0. The visible
window remained black. Evidence: `p3-sysmem/`, including the correctly matched
`report-236.jsonl`; `report.jsonl` is an older PID 240 report and is not evidence
for this run. The test override was restored after capture.

The default-off software-breakpoint gate built successfully as r12, SHA
`9f3caf0797fb1fa41284495f933bf04cbc177533212e4b3c6d544ee321e29f2c`, patch SHA
`16e2e5b07b48a384c8fb2b75334b99b2c27640a4828994a2f4c27a58b1bf598e`.
This is a build result, not a new debugger device acceptance.

## Experimental retained AHB sampling

`XRGAME_AHB_PRESENT=sample` is debug-only and defaults off. Each mapped window
retains its current Present. On replacement, an exclusive render-frame lock
prevents new reads of the old image; a graphics-queue barrier submission and
fence retire existing reads, release the old layout/ownership and acquire the
new AHB. Only then is the old Present eligible for Idle. Unmap, destroy,
resize and surface teardown retire leases before dropping imports. Failed
GPU retirement keeps the lease; duplicate/busy AHB and cross-window imports
are rejected. XR, scanout and frame generation do not use this experiment.

The trace distinguishes `sample_ready` and `sample_retired`; the latter is
required before Idle. Four lease lifecycle unit cases and five analyzer cases
passed. Complete currently acknowledges host image acceptance, not measured
physical scanout. Full presentation timing/MSC semantics remain open.

The first sampling APK SHA is
`bb694a5a63844fd971fba448b918e9360db34a6dadad9d5d08317f36c6e15601`,
same lifetime-loader catalog, renderer Build ID
`36421bccc5e74ef19b0ff30b47b21ea071507eab`; audit: 122 entries, zero errors.
It also records the format in `createWinTexResources`, preventing RGBA images
from repeatedly failing the copy destination's format-reuse check.

AYN app PID 3724 / fixture Android PID 5084, Wine PID 244 displayed an animated
D3D11 triangle through sampling. Home paused the session; returning and using
the app's explicit Resume restored animation. Esc exited normally: 3116 frames,
103514 ms including the background interval, one fixture input event, result 0.
No Wine children remained. Evidence: `p3-sample/` and screenshots 99–101.
The initial trace had 905 complete frames with no ordering diagnostics; the
later logcat tail had 1486 complete and five partial frames, also no ordering
diagnostics. The ring-buffer truncation is retained, not counted as proof of
all frames. No performance improvement is claimed from these noisy samples.

On the same APK, Hades II first failed before graphics initialization. The loader
recorded successful ResumeThread (previous suspend count 1) followed immediately
by process/thread exit **3221225477 / 0xC0000005**, at 521 ms. This disproves the
idea that waiting on the process handle alone fixed the intermittent failure.
The retry entered the menu and The Crossroads using sampling: app PID 7096,
Hades PID 9430. Two actual-scene captures of the same generation were **619.385 s**
apart; keyboard move/dash worked at the end, then Alt+F4 exited with no Wine
children. Its continuous bounded trace contains **3600/3600 complete frames**,
zero nonzero wait fences and zero ordering diagnostics. Evidence:
`p3-sample-hades/`, screenshots 103–106. This proves the tested flat path,
not all window lifecycles, target-MSC scheduling or XR correctness.

A new loader diagnostic revision no longer treats a remote thread's truncated
DWORD exit value as an HMODULE. It checks that the process is alive and the DLL
is present in the target module list; the loader also propagates the child's
exit code. Binary SHA `40834ea7074da07bf3387f08a04e53b7d917cd4c3c29fd1a909e7a1e679218da`,
patch SHA `1489fe2d5ae4f28138731da982ad5a7c9514d838f4867397abbb4b6b621b6b53`.
It was installed with preserved app data in APK
`2a0a1e7ffd323bd0d86bec316a00cd8079b405668d3f419dc25f97833003cec7`
(122 audited entries, zero errors); installed base.apk SHA matched. Hades startup
results on this loader revision are recorded below; the intermittent failure is not declared fixed.

### Preliminary same-scene comparison

The source-owned `capture-present-window.py` records process generation, CPU ticks,
RSS, raw KGSL context and SurfaceFlinger presentation history. Four tests cover
units, overlapping histories, polling gaps, invalid fences and PID reuse.
Both trials used the same APK, 1280×720 Hades Crossroads spawn scene and default
60 Hz presentation, with no input during the selected 45-second interval.
The sample trial's full raw 60-second record includes movement near its end;
only its first 45 seconds are used below. All raw records are retained.

| Measurement | Direct sample | GPU copy |
|---|---:|---:|
| App PID | 7096 | 13921 |
| Hades PID | 9430 | 15680 |
| Host CPU mean, cores | 0.28484 | 0.31016 |
| Guest CPU mean, cores | 1.06669 | 1.07379 |
| Surface actual-present interval mean | 16.71748 ms | 16.71752 ms |
| Surface actual-present interval p95 | 16.72224 ms | 16.72146 ms |
| Host RSS mean | 422.27 MiB | 380.44 MiB |
| Guest RSS mean | 1128.14 MiB | 1124.19 MiB |

GPU frequency reads were 401 MHz in both selected windows. These are sequential,
not randomized trials. The sample process had earlier trace/retry history while
the copy process was fresh; RSS is therefore not an isolated buffer-memory
comparison. KGSL busy counters are shared with other readers and are not used as
attributed GPU time. There is no observed frame-rate improvement, and this single
CPU difference does not establish a repeatable performance gain. Default remains
GPU copy. Evidence: `p3-sample-hades/window-v1{,-stationary-first45}.json` and
`p3-copy-compare/window-v1.json`.

### Resize and hide/show fixture

Fixture v6 (`70df50deec598697a90824a3e9a0cec00c4cdb4d25769264faa0bda4a42b3b1e`)
records its Wine and Android PID using `ProcessWineUnixPid`, rather than relying
on a reused report filename. Its `--lifecycle` mode calls ResizeBuffers through
792×473 → 992×623 → 792×473, hides/shows the window, then exits.
On app PID 18044 / fixture Android PID 19546 / Wine PID 240, all three resize
results were 0, 480 frames completed in 8683 ms, and exit was 0. The full trace
has **480/480 complete frames**, including final retirement, with no ordering
diagnostics. Screenshots show the resized triangle. The first `/proc` snapshot
caught the launcher before the actual fixture existed; the report's successful
Wine Unix-PID query supplies the fixture PID, and that limitation is retained.
No Wine children remained afterwards. Evidence: `p3-lifecycle/`.


The v6 clear-only case still displayed black with direct sampling: app PID 21295,
Android fixture PID 22700 / Wine PID 240, 2447 frames, 40829 ms, Esc exit 0.
The run report records successful device/shader/ResizeBuffers initialization;
no Wine children remained. This remains an unresolved negative case. Probe
arguments/environment were restored to their prior values. Evidence: `p3-clear-v6/`.

WineDbg r13 additionally bounds the expected-Unix-PID environment string before
parsing. Binary SHA `bfa3c8ebe3f908159566e461490d9692d2c40ce8aba0614cc087883d21b9bb4b`,
patch SHA `55a884fb6f81bfdc431ee91aa9df46cbb8bb370a0d1ddb39242830de46ad4eb1`.
Build/copy succeeded; this is not a new device acceptance or a software-breakpoint
resume fix.


### Loader v4 / combined APK retest

APK `29c10bae9737be05d7f3be78bdded2de4f0431fbf63a0ff31e86be405521ec2a`
passed the 122-entry audit with zero errors and matched installed base.apk. Catalog
SHA `260213a29a2d57087273b8edb1a8171c8d6aaaf29604bea8e47e3ecdb5bdb1a7`;
complete library Build IDs and device generations are bound in `p2-combined-artifacts.json`
and `p2-loader-v4/launch*-*.json` outside the repository.

- App PID 5399 / Hades PID 7248: the new loader verified injection, resumed with
  prior suspend count 1, reached The Crossroads from the existing save, accepted
  movement/dash and exited normally. Process wait was 128757 ms, exit 0.
- With temporary SEH logging removed, cold app PID 8028 / Hades PID 9292 reached
  the menu and exited normally: process wait 46905 ms, exit 0. Both runs had no
  Wine children after exit. The loader log appends across sessions; each interval
  is delimited by its own opening record.
- The following UI attempt opened Monster Hunter's detail page after returning
  to the library. It did not start Hades; screenshot 150 and `launch3-running.json`
  are retained as an aborted navigation, not counted as a third startup.

These two successes do not establish that the earlier intermittent 0xC0000005
startup exit has been fixed. Hades environment was restored to its prior values.
Evidence: `p2-loader-v4/`, screenshots 145–151.

### WineDbg r14: continue accepted, guest progress still absent

Binary SHA `36a196f93f42237f23ba7b46915b00ad09c6a73b2e9d4103cf45429dbb5f387c`,
patch SHA `2577ea7540e438f1c12ea85351a066e6636789d24c479c03688d40885ec05508`.
This diagnostic revision skips an unchanged thread-context write when toggling
single-step and logs ContinueDebugEvent results. It does not enable breakpoints
by default or claim an ARM64EC resume fix.

On the same combined APK, GDI fixture v6 ran as Android PID 13338 / Wine PID 240
(app PID 8028). Exact start ticks and provider Unix-PID verification are recorded.
MCP session `gdbg_1c854a62e92741739c639764382b3db7` hit the opt-in breakpoint at
`0x1400013e0`: RCX=2, RDX=0x12345678, RSP=0x11f0d8, EFLAGS=0x242.
After breakpoint removal and plain continue, the proxy recorded unchanged context,
ResumeThread count 1 and successful ContinueDebugEvent (DBG_CONTINUE). No function
return report appeared and Esc did not exit. The earlier unwanted-context-write
hypothesis is therefore insufficient; no single-step success is claimed.

MCP stop released the owned forward and reported cleaned. The guest itself was
still alive and unresponsive; transport cleanup is not proof of guest recovery.
After retaining that process snapshot, the authorized fixture session was stopped;
no app/Wine processes remained. The detach tail includes a failed later continue
(error 5), retained separately from the successful breakpoint continue. Evidence:
`debugger-r14/{controller,controller-stop,after-detach,after-stop}.json`, proxy log
and journal, screenshot 152. Default software breakpoints remain disabled.


### Clear-only isolation and DXVK candidate

A two-line splash correction now uses the flavor's `app_name` resource instead of
hard-coded GameNative text. That APK was built successfully and installed with
preserved data: SHA `176d4fba38317b1eef13fdb63d954c8b9d8bf1236e10c9051e9af8a873ce797d`,
122 audit entries / zero errors, catalog `260213a29a2d57087273b8edb1a8171c8d6aaaf29604bea8e47e3ecdb5bdb1a7`.
The first screenshot caught the earlier loading dialog, not the branded splash;
it is not claimed as visual proof of that text.

- Fixture v7 SHA `60f75b3b7489b6cc1e54e87a5587c28ddb401a30484e943ddfda623c2f6b8f39`,
  source SHA `a77a772954e4ec5640b7dd6ae1472c6caf5102e161c197decddcec3abaee01cd`.
  `--clear-readback` reads one pixel on the first frame only: [26,26,179,255],
  successful HRESULT. App PID 27943 / fixture PID 29958 / Wine PID 244 showed
  a blue window, completed 1958 frames over 32657 ms and exited 0 with no Wine
  children. The initially queried PID-240 report belonged to the earlier GDI run;
  the matched PID-244 report is the evidence used here.
- On v7, `--clear-only` with the `TU_DEBUG=noconform,noubwc` override stayed black:
  app PID 31727 / fixture PID 32754 / Wine PID 240, 3307 frames / 55137 ms, exit 0,
  no Wine children after exit. The override was then removed.
- Fixture v8 SHA `f7545cdcca3b66e51cb28c201fead74e3560cd007df84b58cbdf1cb86db7457a`,
  source SHA `2de56e9e994395ff6a982b5eb2335796195b925dd0897c25d7d065487704d9ca`.
  `--clear-flush` explicitly calls Flush before each Present. App PID 1224 /
  fixture PID 2662 / Wine PID 240 remained black: 2222 frames / 72901 ms, exit 0.
  The first Esc cleanup snapshot still had live Wine processes; the later exit
  report and `flush-final-cleanup.json` confirm eventual cleanup. No zero-residual
  claim is attached to the earlier snapshot.

Fixed DXVK `a6764047e587178283fcde4073ae6e1410af594f` defers render-target clears.
Its external-rendering handoff ends commands with a suspended render pass, flushing
only shared images, while the presentation blitter samples an ordinary backbuffer.
The candidate calls `spillRenderPass(false)` first, which executes pending clears
and restores shader-readable layouts. This is consistent with the readback control;
GPU copy versus direct sampling and UBWC did not explain the observed difference.
The candidate is built from an isolated source copy with all nested pins recorded.

The first candidate APK `68804fd3618025932591c708ae153022d39d64c7a93b06dccbf535c3f882c2a2`
passed the outer APK audit but launch was correctly rejected because the one-off
WCP staging script omitted `xrgame-build.json`. No DXVK execution is claimed on
that attempt. The failed archive/catalog and screenshot 169 are retained. The
incomplete new component directory was moved outside `contents` for recovery,
without touching existing components or game data. The staging script was corrected
and the archive validator then passed (10 members, 9 recorded files).

Raw isolation evidence: `p3-clear-isolate/`, `p3-clear-noubwc/`, `windows-probe-v7/`,
`windows-probe-v8/`, `dxvk-clear-build/`, screenshots 165–170. All process snapshots
bind the APK audit, complete native Build IDs and device generation.


### DXVK deferred-clear fix: device regression passed

The corrected fresh APK SHA is
`591ada4f413a8baedf696109bac87201661b0884caadcfe210338db596c23b6e`;
its installed base.apk matches. Audit: 122 entries, zero errors. Catalog SHA:
`fc0795bb7897b48ab43e55b306fa4b4cc7cad4875845ff9f0cf4289153aeb464`.
The DXVK xrg2 archive SHA is
`c2b801c2e584b793c93deb678bee5c7138740dd5916c37bad324caebc005deee`.
The patched source/build identity is in `xrgame-dxvk-build.json`; local patch SHA
`54c52f1a46c693dfd5740d006beb8843dc8d7baa1f1aaf27b6ec3bc8609ba653`.
The installed probe-prefix DLL hashes match that build:

- d3d11.dll: `95fbdd6812c1996ecde83de6d2de32cdf245b480a2cdbc6eea6a6f1f892dfc82`
- dxgi.dll: `b7165521f0731949d1569bb9c0b716ca02704a03a1c76b1cb442e84b95ff12cb`

The same v8 fixture used above now displays changing clear colors in **both**
AHB GPU-copy and direct-sampling modes. These runs use `--clear-only`, without
CPU readback or explicit per-frame Flush. Full process snapshots bind device
identity, boot/start generation and all APK library Build IDs.

| Run | App / Android fixture PID | Frames / duration | Result |
|---|---|---|---|
| Default copy, timed exit | 11853 / 12897 | 7204 / 120067 ms | exit 0, no Wine children |
| Default copy, repeated animation | 11853 / 16317 | 1180 / 19664 ms | three differing screenshots, exit 0, no Wine children |
| Direct sampling | 16889 / 17911 | 750 / 12530 ms | three differing screenshots, exit 0, no Wine children |
| Sampling triangle lifecycle | 18606 / 19621 | 480 / 8666 ms | three successful resizes, hide/show, exit 0, no Wine children |

All four reports identify Wine PID 240 and their distinct Android PIDs. The
sample-clear trace has 120/120 complete frames; lifecycle has 480/480, including
final retirement. Neither has an ordering diagnostic. All observed wait-fence
IDs are zero; this does not establish nonzero wait-fence or target-MSC behavior.
Screenshots are display evidence, not a pixel-exact colorimetry test.

Hades II additionally reached The Crossroads, accepted keyboard movement/dash
and exited normally on app PID 20459 / Hades PID 21534. The loader recorded a
186546 ms process wait, process/thread exit 0; no Wine children remained.
The initial black screenshots caught startup/loading before later menu/scene
screenshots. This regression run does not close the earlier intermittent startup
failure. Probe environment/arguments were restored; default rendering remains GPU
copy, with sampling opt-in only.

Runtime staging now validates archive internals before copying assets, so a
missing `xrgame-build.json` is rejected before APK packaging. Six package-validation
tests passed, and all seven current archives staged successfully. Evidence:
`p3-clear-isolate/`, including the rejected first package, animation/lifecycle
screenshots, trace analyses, installed hashes and Hades loader log.


### Breakpoint stall: additional SEH / kernel-wait evidence

The same r14 debugger was retested with fixture v8 and bounded explicit `+seh`
logging on APK `591ada4f…`. App PID 22741 / fixture PID 23787, Wine PID 232;
the report was selected by its Android PID, not the reused Wine report name.
MCP session `gdbg_d5bfcba5fe884b1fb8c6a2c7372a4a24` again hit
`xrgame_probe_add` at `0x1400013e0` (verified against the exact v8 PE symbol table),
with RCX=2, RDX=0x12345678 and EFLAGS=0x242. Removing the breakpoint and continuing
was accepted, but the function did not return and Esc did not exit.

The SEH log now records Wine's guest/ARM64EC unwind through the fixture,
user32, kernel32 and ntdll. After continue, `/proc` reports the main thread blocked
in syscall 98 (futex), with `futex_wait_queue_me`; the wait address is in anonymous
memory. This proves a wait, not which mutex or owner caused it. Reading the kernel
stack was denied. No raw ptrace controller or process-name bypass was used.

MCP cleanup released its forward with `guest_stop_timeout`; the fixture remained
alive. The authorized test session was then force-stopped, with no remaining app
or Wine processes. Temporary Wine logging was removed. Raw evidence:
`debugger-r14-seh/`, including controller events, provider journal, matched report,
SEH log, maps/syscall snapshots and pre/post cleanup identities.

### FEX cross-process invalidation: root cause and recovery fix

The private bounded context trace first failed during startup because its own
C++ TLS access ran before PE TLS initialization (`libarm64ecfex` RVA `0x10b3d8`).
That rejected diagnostic APK SHA is
`d4e1e5569e5707bbeb0d4bb327840d7577316e19db6474e99f9fe1660700ec8d`;
its launch log and cleanup are retained in `fex-context-trace/`. No target behavior
is inferred from that instrumentation failure. The revised trace uses bounded
atomic state, with no TLS access, and launches successfully.

Trace v2 APK SHA `014a2b7906eedb96e2187c7c5318feaf7ea770715ecb7589894186f5a7d992b8`,
catalog SHA `3efc28bba493a000363794a6693ea3a68042860c37466d34593c7af1e7deb169`,
app PID 10154 / fixture PID 11212 / Wine PID 244, reproduced the stall. The trace
shows `SyncThreadContext` entering cross-process work, a protection notification,
and a nested `BTCpu64FlushInstructionCache` on a JIT address before the protection
notification returns. Source inspection explains the cycle: invalidation holds
`CodeInvalidationMutex`, delinks native branches and flushes their instruction
cache; Wine reenters FEX invalidation because `InSyscallCallback` was not set.
The outer thread mutex is recursive, but the code-invalidation mutex is not.
The futex wait and unfinished trace are retained in `fex-context-trace2/`.
MCP detached with a stop timeout; the still-stalled fixture was then stopped.

`fex-arm64ec-work-callback-guard.patch` wraps all three pending-work call sites
with FEX's existing `ScopedCallbackDisable`, restoring the previous callback
flag on return. This protects the native JIT flush from recursive emulator
notifications; it does not discard the original cross-process work.
The patch remains local to the owned fork build recipe, not upstream FEX.

Guard-only validation identity:

- APK SHA: `13a25fbb733fd87fe2ba879bea7d9359d8015d06be60d1206e0a312abaf592a6`
- catalog SHA: `fbe034ec5f10fb5a750f69cccddbcdeac2ae7864c4b1f85593b236f4222c33d8`
- FEX DLL SHA: `ae9cc804f9832d17e2cdf1625904f6b44324a49d84f30c37b7474b0d7acb2f33`
- patch SHA: `c247de9e231b06e8bcd5dec83c904633bae4c4dbc521640016b80dfe1be3d907`
- app PID 19200 / fixture PID 20229 / Wine PID 240; same v8 fixture.

The future breakpoint at `0x1400013e0` hit with RCX=2 and RDX=0x12345678.
After removal and continue, the report records the correct result `0x1234567a`.
A second hit then stepped PUSH, MOV, LEA and POP through RIPs `13e1`, `13e4`,
`13e8`, `13e9` (relative to `0x140000000`), with matching RSP/RBP and result
`0x1234567b`. A third hit at CALL `0x1400026dd` stepped exactly to `0x1400013e0`,
then continued to result `0x1234567c`. These are observed stops, not merely accepted
step requests. Register mutation was not tested through MCP.

RET stepping is still incorrect in this guard-only build: it stops later in
ucrtbase at RVA `0x1306d0`, after executing beyond caller `0x1400026e2`.
Source inspection points to the cached shadow-return path bypassing the trap
check. This remaining bug is separate from the repaired invalidation deadlock.
Software breakpoints remain fixture-only and opt-in while D2 is incomplete.

The MCP session released ownership with no warnings, and the fixture still
responded. Normal Alt-F4 exit returned 0 after 208418 ms, with no Wine children.
Temporary target logging was removed. Raw evidence is in `fex-work-guard/`, with
controller epochs/registers, provider journal, target SEH log, report and process
snapshots binding the full device generation and APK native Build IDs.

Guard-only Hades II regression also passed: app PID 23674 / Hades PID 24725,
installed game-prefix FEX SHA matched `ae9cc804…`, reached The Crossroads and
accepted movement/dash. The loader recorded 354350 ms through process exit 0;
normal Alt-F4 left no Wine processes. This is a fresh runtime regression, not a
new ten-minute endurance claim. Evidence: `fex-work-guard/hades-*`.


### FEX return stepping: exact CALL/RET chain verified

`fex-single-step-return.patch` (SHA
`903516ae2cf8dcf710652c493a9e81d23a328319ee1747aea78b16eb144feec6`)
keeps the shadow stack balanced but routes a TF-compiled RET through the dispatcher,
so the next instruction receives a trap check. This is selected at compile time;
ordinary non-stepping return code keeps its existing cached path.

Combined validation APK SHA
`e8b3cafd41b21abfdda596ea7ee6bbfb3aeb58afb41a7d3cbd589927298e7207`, catalog SHA
`eb8c5f8f95cc79248fa55d496a86ca7f42e68351aa1052b98361141d262df2c8`, FEX DLL SHA
`69d49b312899f5c68a907544edc3cdad63c6016340c8e983d59612fee4e945f2`.
App PID 26451 / fixture PID 27500 / Wine PID 240, x64 thread 244.
MCP session `gdbg_433e43144c6742db92bc984a876cb17d` observed:

| Instruction just executed | New RIP | RSP | RAX |
|---|---|---|---|
| CALL at `0x1400026dd` | `0x1400013e0` | `0x11f0c8` | `2` |
| PUSH RBP | `0x1400013e1` | `0x11f0c0` | `2` |
| MOV RBP,RSP | `0x1400013e4` | `0x11f0c0` | `2` |
| LEA RAX,[RCX+RDX] | `0x1400013e8` | `0x11f0c0` | `0x1234567a` |
| POP RBP | `0x1400013e9` | `0x11f0c8` | `0x1234567a` |
| RET | `0x1400026e2` | `0x11f0d0` | `0x1234567a` |

The exact PE disassembly agrees with all six RIP transitions. A continue request
using old epoch 7 at current epoch 8 was rejected as `stop_epoch_stale`; current
epoch continue succeeded and the fixture recorded the result. After clean MCP
release, another key-triggered call returned `0x1234567b`. Alt-F4 then exited 0
after 107328 ms, with no Wine children or owned forward remaining.

This closes the reproduced deadlock and this single-thread CALL/RET control case.
It does not establish arbitrary register writes, multithread stepping, guest
watchpoints or full mixed unwinding. Those D2/D1 gaps remain explicit; fixture
breakpoints are still opt-in. Full raw records: `fex-work-guard-ret/`.

The combined xrg3 trial also passed a Hades II regression (app 31211 / Hades 32315):
scene rendering, keyboard dash, process exit 0 after 326728 ms, and no Wine children.
Its installed game-prefix FEX SHA matched the combined trial DLL.

Normal-build integration rebuilt both PE modules and Unix libraries. The first
attempt stopped because the private staging directory lacked `fex-unixlib`; the
missing source subtree was staged and the Unix-lib CMake cache refreshed before
retry. Both build logs remain in `fex-work-guard-ret/`. The first normal xrg4 APK,
SHA `eae8d614c500efc3d4850454a8d9907a77c07be6790e8cafa5abfa51a11a7449`, passed
122-entry audit and the same six-step CALL/RET sequence on app 2233 / fixture 3393
(Wine PID 232). The installed APK hash was checked independently. Two correct
function results, clean MCP release and exit 0 after 109937 ms are recorded in
`fex-final/`. The normal build's PE hashes differ from the incremental candidate,
so this repeat used the actual rebuilt DLL, not an assumed equivalence.

A final source review retained the original shadow-stack `ldp` guard-page access
in the stepped RET path, replacing the initial pointer-only pop. This preserves
the frontend's overflow/underflow fault handling. That refinement is versioned
separately as xrg5; earlier captures remain tied to their original patch hashes.

### Current default: FEX xrg5

The default source recipe and app version now select `2608-3f1f30a-xrg5`, alongside
DXVK xrg2. The production remote catalog is still unpublished; these are internal
bundled components. Source pins and child checkouts were not advanced.

| Identity | SHA-256 / Build ID |
|---|---|
| APK (also checked against installed base.apk) | `5840b81de3358741bf6d93b83979fff966b5ab9253102da1de778a24df9d468e` |
| Bundled catalog | `90eaac4a0bb18ecaa1d517f0ea2a3a7544e486efc2cd9cc8114b08a63de056e2` |
| FEX xrg5 archive | `cb315ffd4ae8decf1bb39aac7e88059d5bb34b565196a652238543d5954b781b` |
| ARM64EC FEX DLL | `cb37571dbb1d6059939e1609c6c5bcbad75fa48a44ebcb458d9dfa53f7d17fd9` |
| Final RET patch | `4ac75be455fd52ff006d08c66142d8d39f9b211a1a0896c58d976e69cdb55d7d` |
| ARM64EC Unix library Build ID | `6f7b194ca15d2b03f1fa964a45220f65d6cf5d52` |
| WOW64 Unix library Build ID | `49ff65bcf1680c7b02c587e7ab1c7e7cc073b5a6` |

Fresh packaging passed 122-entry APK audit with zero errors. Runtime staging
verified archive inventories and hashes. The targeted host suite has 41 passing
tests; Python compilation, shell syntax, local document links and diff whitespace
checks passed. The first normal-build attempt and all intermediate APK/debugger
failures remain retained; no clean-rebuild or complete-release claim is added.

On the installed xrg5 APK, app PID 8345 / fixture PID 9382 / Wine PID 236 repeated
the same six precise CALL/PUSH/MOV/LEA/POP/RET stops. MCP session
`gdbg_736710c56c724ffeb1b2c54ddbe9897c` released ownership without warnings; the
fixture returned `0x1234567a`, then `0x1234567b` after detach, and exited 0 after
92825 ms. No Wine children remained. The normal non-diagnostic container settings
were restored. Evidence: `fex-xrg5/`, including all APK ELF Build IDs, the full
four-library build record, exact device generation and controller epochs.

Final xrg5 Hades II regression: app PID 11051 / Hades PID 12116, game-prefix FEX
DLL SHA matched the final build, The Crossroads rendered, and keyboard movement/
dash worked. Normal Alt-F4 produced process exit 0 after 169000 ms, with no Wine
children. Debug logging was removed, no adb forward or active scrcpy session
remained, and the device was returned to Home with its screen asleep.
