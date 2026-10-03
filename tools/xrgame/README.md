# XRGame runtime builds

This pipeline builds the pinned Proton 11 ARM64EC, FEX, DXVK, VKD3D-Proton,
Turnip, XCB, PulseAudio and base imagefs used by picoXr. It creates draft evidence;
it does not publish a Release, upload an APK or change the production catalog.

## Fresh Linux build

Use Linux x86_64 with Docker and enough data-disk space. On the configured Linux
host, keep all build roots under `/data00/xrgame-native`. Choose a new root each
time; the entry point refuses an existing directory and freezes recipe inputs.

```bash
bash tools/xrgame/run-runtime-container.sh \
  /data00/xrgame-native/build-<run-id> \
  /data00/xrgame-native/downloads
```

The optional second argument supplies only cached download archives; their
fixed SHA-256 values are checked before extraction. No prior build products,
toolchain installation or source checkout is reused by this entry point.
The initial Docker image installs the build dependencies in `Dockerfile`.
`XRGAME_JOBS` controls compiler concurrency. `XRGAME_BASE_IMAGE` can name an
equivalent locally imported Ubuntu 24.04 image; its resolved identity is recorded.
Ubuntu packages are version-recorded, not snapshot-pinned, so this does not claim
bit-for-bit reproducibility across machines or dates.

For a host-local proxy, pass `XRGAME_DOCKER_NETWORK=host` and the proxy environment
variables for this command only. Use an HTTP proxy endpoint during bootstrap:
pip does not yet have the optional PySocks dependency. No proxy or credential is
written to the repository. Git downloads can resume after an interrupted fetch;
an existing checkout at the wrong pin or with tracked modifications is rejected.

`.github/workflows/xrgame-runtime.yml` runs the same preparation/build/source
recording scripts on a new Ubuntu runner. It is manual and stores only runtime
archives and evidence. The existing picoXr build workflow remains responsible
for the APK and app tests.

## Outputs and source scope

| Path under build root | Contents |
|---|---|
| `logs/` | Phase timestamps, compiler/setup logs, Ubuntu package versions, image identity |
| `packages/runtime-20260926-preview1/` | Six runtime archives, draft catalog, build record and SHA256SUMS |
| `sources/` | Exact Git source trees, initialized submodules, ntsync Rust vendor, XCB sources and recipes |
| `dependency-sources/` | Version-matched Termux source archives, recipe tree/overrides and imagefs record |

`collect-runtime-sources.py` checks Git pins and tracked changes before exporting
source. Each submodule omitted from the build is listed explicitly. It does not
archive `.git` metadata, credentials, APKs or game data. `finalize-runtime.py`
checks the packaged recipe hashes against the source archive index and binds
the six component hashes, source archives and catalog.

`termux-sources.lock.json` covers the 81 packages selected by the current imagefs.
The default recipe snapshot matches 80 installed versions; libwebp uses the
older `1.6.0-rc1` recipe at its own commit. PulseAudio's Git tag is resolved to a
commit and the exported archive is SHA-pinned. Recipe-local source is included
for packages with no separate upstream archive. Source download hashes must
match even when a declared mirror is used.

The prebuilt Termux snapshot does not attest each original build commit.
Version matching therefore does not establish exact binary provenance. Additional
build-time downloads (including librav1e's Rust dependencies and ICU's license
replacement), NDK source attribution, and rebuilding from the complete source
collection remain open. The source indexes and build record retain
`completeCorrespondingSource: false` / `releaseReady: false` until these are resolved.

## Android X11 presentation

`build-turnip.sh` applies `patches/turnip-x11-ahb.patch` to the pinned shadPS4
Mesa baseline in a build directory keyed by the patch hash. The private xrg5
package includes source-built XCB DRI3/Present dependencies as well as the ICD.
The app verifies these files before deploying them into imagefs. Do not test a
new ICD alone against an older base image without checking its dynamic dependencies.

picoXr selects its Vulkan display renderer and enables `XRGAME_X11_AHB=1` after
merging the container environment. Guest rendering finishes before X11 Present;
the host copies the shared AHB on the GPU and sends Complete/Idle after its fence.
There is no CPU readback in this path. Do not carry the older xrg3 `sw,noshm`,
forced `immediate`, or disabled present-wait/id settings into AHB acceptance.
The measured Hades II results and failures are in
[`wp3-hades2-20260926.md`](../../docs/validation/wp3-hades2-20260926.md).

Each launch creates an isolated `NTSYNC_SHM` after stale Wine processes stop.
The guest-debugger helper inherits that exact value from its selected process;
starting a debugger with the default registry would join a different sync domain.

## Device debugging (private validation)

The native and guest smoke record is
[`docs/validation/wp3-debuggers-20260926.md`](../../docs/validation/wp3-debuggers-20260926.md).
Use the `native_debugger` MCP as the sole live controller; keep logs outside this
public repository. A successful debugger test does not accept a game or W3.

For Android app native attach, select the authorized device explicitly and inspect
the app PID with `maxModules: 1024`. Pass the resulting target token to
`android_debug.start_session`. The tested macOS ARM64 toolchain is the complete
AGDE `26.1.102-ndk-29.0.14206865-lldb21-osx-arm64` bundle: its
`adapter/codelldb`, `host/lib/liblldb.dylib`, and `server/arm64-v8a/lldb-server`
must be used together. Do not mix the VS Code adapter with another LLDB library.
Pause, wait for the stopped event, inspect at its exact epoch, continue, export
evidence, then stop the session and verify `TracerPid: 0`. This route was tested
on the app process; the package-bound attach API rejects Wine child command names.

For Windows x64 guest attach, build the optional debugger on the Linux data disk:

```sh
docker exec xrgame-wp3 bash /work/project/tools/xrgame/build-guest-debugger.sh
```

It exports the pinned Proton Wine source, applies `patches/winedbg-local-guest.patch`,
and builds only an x64 WineDbg PE. Outputs are `output/debugger/xrgame-winedbg-x64.exe`
and `xrgame-winedbg-build.json`; neither belongs in the APK or production catalog.
The child maintenance branch is `feature/malos/wine-guest-debug`, based on the
original `5d0d333e` runtime baseline. It was published and rebased onto the owned
`proton_11.0-2` branch for the 2026-09-28 source checkpoint. The recipe deliberately
retains its validated pin and patch, independently of the newer maintenance
gitlink. Build copies are keyed by pin and patch hash so an old patched source
directory is never silently reused.

After starting a game, inspect its Android PID/start ticks and the separate Wine
process ID from the sanitized runtime log. Copy the debugger to the workstation
and run this helper in a terminal, substituting freshly inspected values:

```sh
python3 tools/xrgame/debug_identity.py --serial "$DEVICE_SERIAL" --pid "$GAME_PID"

python3 tools/xrgame/run-guest-debugger.py \
  --serial "$DEVICE_SERIAL" --pid "$GAME_PID" --start-time-ticks "$GAME_START_TICKS" \
  --boot-id "$DEVICE_BOOT_ID" --uid "$APP_UID" --evidence-dir "$FRESH_PRIVATE_DIR" \
  --wine-pid "$WINE_PID" --wine-loader "$DEVICE_WINE_LOADER" \
  --debugger "$LOCAL_WINEDBG" --fex-exceptions
```

Use the boot ID, UID and start ticks from that read-only inspection. The helper
checks boot/process generation and app UID before staging and again before launch,
rejects an existing native tracer, resolves prefix/loader paths within the selected
app, copies a SHA-named executable into the
target prefix's `C:\xrgame-debugger`, verifies its SHA, and starts the loopback
proxy. It copies only an environment allowlist and does not create an adb forward.
The fresh evidence directory must be outside the repository. Its `journal.jsonl`
retains failed attempts and final target identity without recording environment
values. Wine PID is caller-supplied and still needs provider verification: this
helper passes the expected Android PID, and the matching WineDbg patch validates
it with `ProcessWineUnixPid` before attaching. Use that matching debugger build;
older binaries do not implement this guard. A proxy exit or zero
`TracerPid` is not a cleanup receipt; verify the owned resources below. Stopping
the local helper alone does not guarantee that the remote WineDbg has detached.
Use the guest MCP with `serial`, `port: 41731`, `remotePort: 41731`,
`expectedArchitecture: "i386:x86-64"`, **`byteOrder: "little"`**,
`hostPid`, `hostStartTimeTicks`, `stopOnAttach: true`, and `dryRun: false`.
Then explicitly pause and use the returned stop epoch for registers/memory/control.
The initial handshake may report `connected_unknown` until that pause.

The patch handles interrupts while already stopped, resolves remote break-in from
the target's own ntdll exports, and detaches on disconnect both while stopped and
while waiting for a running target. `--fex-exceptions` passes first-chance access
violations to the application's exception handlers and passes FEX's exact
`brk #0xCAFE` suspension trap and `brk #0` guest-trap conversion; second-chance faults still stop. Omit this option
when investigating first-chance access violations themselves. Its register view
is Wine's x64/ARM64EC context; this is not FEX's Linux mixed-stack provider.

End with `guest_debug.stop_session`. Verify the helper exits, WineDbg disappears,
the owned forward is removed, and the game PID/start ticks remain unchanged.
Keep failed captures as well as successful results. Breakpoints, stepping and
mixed guest/host unwinding are not covered by the current smoke acceptance.
The fixture-only `--experimental-software-breakpoints` implementation keeps original bytes, flushes the
instruction cache, and restores bytes on removal/detach. At an owned breakpoint,
remove it before continuing or stepping over that instruction. Do not interpret
an accepted step packet as an observed step stop. The original FEX build hangs after breakpoint removal. The callback guard repairs recovery, and the separate return-cache fix restores
exact CALL/RET stepping in the fixture. Both are included in FEX xrg5. Keep Z0 insertion opt-in until the D2 matrix is
complete, and bind every trial to its FEX DLL SHA. `--control-trace` records suspend/resume counts. Default plain
continue resumes all threads; an explicit Hc selection is honored.
The original-spec mapping and next debugger milestones are in
[`spec-observability-review-20260926.md`](../../docs/validation/spec-observability-review-20260926.md).

## Offline local Steam IPC validation

The private GBE client build uses `7a319f0bedad260f952b0fb367b27f255fd952c5`
plus `patches/gbe-offline-local-ipc.patch`. The patch keeps same-process message
queues and callbacks working with `disable_networking=1`; remote sockets and
announcements remain disabled. It fixes Alyx's local client/server handshake
returning `k_EResultFail` during map loading. See the
[VR validation](../../docs/validation/vr-sbs-20261002.md) for device scope.

Apply the patch to the pinned source (not the later maintenance branch), then
rebuild the existing `steamclient_experimental` x64 Release MSVC project. Preserve
the original dependency/source build identity, baseline `dll/network.cpp` SHA,
patch SHA and output DLL SHA in the private `source-built-offline-local-ipc` build
record. `package-steamclient-validation.py --client-build-record <record>` checks
the base/patched identity and embeds this record into the component provenance.
The separately recorded loader and extra DLL remain unchanged.

`probes/steam-local-ipc.cpp` loads the client through `SteamClient020` and tests
`SteamNetworkingSockets012`: socket pair, poll group, zero/23/65536-byte payloads,
message numbers, reverse traffic and peer closure. Build against
`references/gbe_fork/sdk`, then run in an isolated directory whose
`steam_settings/configs.main.ini` sets `offline=1`, `disable_networking=1` and
`disable_overlay=1` (the last key belongs to `[main::general]`). Use the original
and patched DLL with the same probe; retain both results. This is local IPC
validation, not multiplayer or Steam authentication acceptance.

## Private source-built Steam loader

Export the fixed loader source subset, then transfer it and the build recipe to
the Linux data disk. The recipe checks the archive SHA, applies the local lifetime
patch, and uses the same pinned llvm-mingw toolchain as the runtime:

```sh
git -C references/gbe_fork archive -o "$PRIVATE_OUTPUT/gbe-loader-source.tar" \
  7a319f0bedad260f952b0fb367b27f255fd952c5 \
  tools/steamclient_loader/win/ColdClientLoader.cpp helpers libs/utfcpp libs/simpleini LICENSE
bash /work/project/tools/xrgame/build-steamclient-loader.sh /work/inputs/gbe-loader-source.tar
```

`output/gbe-loader/loader-build.json` records the source, patch, compiler, recipe
and executable hashes. Pass it as `--loader-build-record` to
`package-steamclient-validation.py`, together with the matching executable and
the separately recorded source-built client DLLs. Stage the resulting archive
through a private catalog; do not substitute it into a published catalog.
`XRGAME_MINGW_CXX` can select a local llvm-mingw compiler when the Linux host is
unavailable; retain its release digest and build record. The loader's remote-export
patch resolves the child process's exported entry point instead of reusing a
process-local ARM64EC import thunk. Record failed validation candidates as well.
Debug loader logs remain inside the prefix's SHA-named client directory.
Complete public-source and dependency-license evidence is still required by the
release audit. Device results and open failures are in
[`personal-p3-debuggers-20260927.md`](../../docs/validation/personal-p3-debuggers-20260927.md).

MHW's `WineBootstrap=1` uses `wine-image-bootstrap.patch`: only the exact configured
game image loads its pinned bootstrap DLL, after TLS initialization and before its
entry point. The environment is inherited by self-restarts. CCL assigns the game
to a job before resuming it, preserves the client registry while descendants live,
and reports the latest observed same-image successor's exit code. It never infers
a restart request from a nonzero exit code. The app leaves this mode off for other
profiles and rejects Proton inventories without `xrgameBootstrapVersion=1`.
Bundled Windows-client launches also set `PROTON_DISABLE_LSTEAMCLIENT=1`, including
after Proton/prefix repair, to avoid redirecting this client into the Linux bridge.

`build-wine.sh` applies the bootstrap patch to both fresh and reused source trees.
For incremental private validation, `build-wine-bootstrap.sh` rebuilds the two PE
ntdll modules from the configured Android Wine tree and records their hashes.
`package-wine-bootstrap-validation.py --base <verified-proton.wcp> --build <output/wine-bootstrap>
--output <candidate.wcp>` binds them to the original archive, patch and build record.
The full `package-components.py` path also requires the rebuilt modules. Neither
path changes the production catalog or closes corresponding-source publication.
`fixtures/restart-probe.c` exercises bootstrap/TLS order, explicit self-restart,
client lifetime, unrelated-child exclusion, normal exit and an error exit.
See [`mhw-restart-20260927.md`](../../docs/validation/mhw-restart-20260927.md).

## Publication check

The complete runtime bundle is now required for every picoXr APK. In addition to
archive hashes, staging checks the Steam client's provenance and the full DXVK /
VKD3D payload, including Proton's D3D10 / 10.1 entry DLLs and shader compiler.
Gradle and `tools/audit-apk` reject missing defaults or missing embedded archives.
This also means an unprovisioned CI runner cannot produce an installable shell APK;
stage a complete verified bundle first. Publication remains a separate optional gate.

On launch, corrupt installed component trees are replaced from authenticated APK
archives with rollback on failure. The operation is blocked while Wine is running.
Prefix graphics DLLs are installed atomically and verified without replacing the
prefix or touching game files. `windows-probe --d3d10-check` exercises both
D3D10 and D3D10.1 hardware-device creation. `XRGAME_MINGW_CC` selects a local
compiler for this source-owned fixture when needed.

For path/API diagnosis, `windows-probe --path-contract` checks module/current
directory queries, UTF-8 conversion and a temporary INI read/write roundtrip. It
reports each result and removes only its own temporary INI file.

For display/buffer compatibility diagnosis, `windows-probe --display-contract`
records Win32 sizes, display enumeration, DXGI modes/descriptors, and matching
16-byte/zero-byte structured SRV/UAV buffer requests. `--display-contract-auto`
requests a zero-sized swapchain and checks that DXGI resolves it to the client
area. `--buffer-contract` needs no window and records the two buffer results
without assuming zero must fail; it tries hardware then WARP and records which
driver was used. Compare its JSONL report under native Windows and Wine rather
than treating a DXVK-only result as the Windows API contract.

```bash
python3 tools/xrgame/audit-runtime-release.py \
  --packages <build-root>/packages/runtime-20260926-preview1 \
  --sources <build-root>/sources \
  --dependency-sources <build-root>/dependency-sources \
  --output <build-root>/logs/release-audit.json
```

This check currently fails as expected on the validation packages. It rejects
missing source prerequisites, changed source indexes/binaries/catalogs, incomplete
component sets, non-owned component URLs and APKs. Never flip readiness fields
just to make it pass. Publication and production-catalog activation follow only
after the recorded prerequisites are actually met and the user authorizes them.

The internal device APK can still consume a private, hash-pinned draft via
`prepare-runtime-bundle.py`. Keep that validation path separate from publishing.

### Experimental flat AHB sampling (personal P3)

On a debug picoXr container, `XRGAME_AHB_PRESENT=sample` retains the current guest
AHB for host sampling. The default remains GPU copy. Set
`XRGAME_PRESENT_TRACE_FRAMES=3600` only for bounded diagnostics; analyze the
capture with `analyze-present-trace.py`. A complete sampling record requires
receive → sample_ready → Complete → sample_retired → Idle. `sample_retired`
is emitted only after the host queue fence and removal of future reads.

This is limited to the current flat Vulkan path, zero-offset / zero-wait-fence
presents, without XR, scanout or frame generation. Surface loss falls back to
the copy path after retiring the retained frame. A busy or aliased buffer is
rejected, and failure retains ownership rather than fabricating Idle.
Complete is currently a host-acceptance timestamp, not physical presentation;
MSC/target scheduling, real GPU timestamps, and like-for-like performance
comparison remain outstanding. Keep the flag off for normal play until the
lifecycle and game comparisons are complete. Device evidence is in
`docs/validation/personal-p3-debuggers-20260927.md`.

For a quiet comparison, use `capture-present-window.py --serial "$SERIAL"`
`--app-pid "$APP_PID" --guest-pid "$GAME_PID" --seconds 60`
`--apk-audit "$PRIVATE_AUDIT" --output "$PRIVATE_NEW_JSON"` while the same scene
is stationary. It binds boot/PID/start ticks, retains partial failures, and
records CPU/RSS plus SurfaceFlinger history once per second. It deduplicates
overlapping histories and does not mistake polling gaps for long frames.
The SurfaceFlinger columns follow
[AOSP FrameTracker::dumpStats](https://android.googlesource.com/platform/frameworks/native/+/cdb6b16dec3a541b455be99d075004cb2f0a0cd7/services/surfaceflinger/FrameTracker.cpp):
desired present, actual present and frame-ready times. KGSL busy counters can be
reset by other readers, so they are retained as raw context, not attributed GPU
time. Both paths need the same sampling settings; don't compare an instrumented
startup trace with a quiet game scene.

### Steam whole-file content check (offline)

`check-steam-manifest.py --manifest <cached-depot.manifest> --sha1-list <inventory.txt>
--root <exact-install-root> --output <private-report.json>` compares a `sha1sum` inventory
against the manifest's whole-file hashes. For encrypted filenames it matches the SHA-1 of
lower-case Windows relative paths; no depot key or account credentials are needed. Generate
the inventory after stopping downloads/verification, using `find <root> -type f -exec sha1sum
{} +` under the app's device identity. Use the exact same absolute root in both commands.
The report rejects missing, altered or unsupported symlink entries. Additional files are
listed but allowed because other depots/runtime files can share the root. Escaped/newline
filenames and duplicate case aliases are rejected rather than silently misinterpreted.

Keep inventories and reports outside this public repository. This checks a cached manifest;
it does **not** validate Valve's signature, obtain an independent manifest, or fulfill the
separate DepotDownloader-download comparison. The real app/depot/manifest IDs and APK/device
capture must accompany a device validation record.


### DXVK deferred clears (ARM64EC)

`build-d3d.sh` now builds DXVK through `build-dxvk-validation.sh`. It archives the
fixed DXVK commit and each pinned nested checkout into a build source copy, applies
`patches/dxvk-present-deferred-clears.patch`, and records archive/patch/recipe/DLL
hashes in `xrgame-dxvk-build.json`. The original checkouts stay unchanged.
The current private validation component is `11.0-a676404-arm64ec-xrg4`; package-components rejects DLLs
or a patch that do not match the build record and includes the patch and record.
The normal `xrgame-build.json` file inventory is still required for installation.

The patch ends deferred clears and restores image layouts before external rendering
uses the command list. Plain D3D11 `ClearRenderTargetView` → `Present` is the regression
fixture; `--clear-readback` and `--clear-flush` are diagnostic controls. Readback is
fixture-only and is not part of the display implementation. Build and device results
are recorded in `docs/validation/personal-p3-debuggers-20260927.md`; the source-build
record alone does not establish device correctness or public-release readiness.

### DXVK frame completion and bounded latency diagnostics

The xrg4 recipe additionally applies `dxvk-waitable-frame-latency.patch` and
`dxvk-present-gpu-completion.patch`; their SHA-256 values enter the build record
and package inventory. `package-dxvk-latency-validation.py` can assemble a private
incremental component from the exact verified xrg2 archive plus rebuilt DLLs.
It verifies the base SHA, source pin, patches, recipe and every DLL before
publishing the local archive. This is not a public runtime release.

- Per-game Graphics → **Render ahead (experimental)** persists
  `XRGAME_DXVK_FRAME_COMPLETION=gpu`; unset or `present` uses the normal path.
  Restart is required. Default off, D3D9–11 only. It selects DXVK's existing
  GPU-completion frame-latency fallback, allowing the next frame before the
  presentation wait completes. Vulkan/X11 Complete/Idle and image reuse remain
  real. Extra queued work can increase input latency, which has not been measured.
- `XRGAME_DXGI_FRAME_LATENCY_TRACE=1` logs swapchain flags, buffer count, effective
  latency and the first eight SetFrameLatency calls. Also set `DXVK_LOG_LEVEL=info`
  and an explicit writable `DXVK_LOG_PATH` while diagnosing; ordinary logging is off.
- `XRGAME_DXGI_FRAME_LATENCY=1|2` is a separate waitable-swapchain diagnostic override.
  It is unused for AI LIMIT: that game already sets latency 2. Invalid requests
  still fail the original API validation. Unset leaves the API behavior unchanged.

These changes do not alter Wine/FEX/Turnip pins. See
`docs/validation/ai-limit-present-pipeline-20261002.md` for bounded device evidence,
including the separate default-off host copy submission/retirement experiment.
The CPU Present pacer now sleeps while its queue is empty and wakes on enqueue
or shutdown; a pending earlier deadline also wakes a timed wait.

For AI LIMIT's private loading experiment, `DXVK_CONFIG=dxvk.maxMemoryBudget=3072`
sets an allocator **soft** budget in MiB. It is neither a hard memory limit nor
the reported VRAM size, and is not a global default. Android LOW_MEMORY exits and
successful replays remain in the evidence. picoXr-generated inline `DXVK_CONFIG`
now uses raw, semicolon-separated values as required by DXVK's parser and execve;
the upstream flavors keep their existing formatting.


### FEX context-resume diagnosis (private fixture only)

`build-fex-context-trace.py` builds an optional ARM64EC FEX DLL from the existing
pre-fix xrg1 Linux build (only its 4 KiB patch) and restores the cached source afterwards. Run it alone, with
no concurrent `build-fex.sh`; its output stays in `output/fex-context-trace` and
is never installed or packaged automatically. It checks the source pin and existing
4 KiB patch, records its own diagnostic patch and DLL hashes, and leaves child
repository pins unchanged. A subsequent normal build recompiles the restored source.

In that diagnostic DLL only, `FEX_SILENTLOG=0` and `XRGAME_FEX_TRACE_RIP=<hex RIP>`
trace context synchronization and cross-process invalidation for the selected
instruction. At most 64 events are emitted. No PE TLS is accessed by the trace:
Wine may invoke protection callbacks before DLL TLS initialization. Use a separately
hashed private runtime catalog/APK for device trials, retain failures, and restore
the regular APK and container environment afterwards. This is diagnosis support,
not acceptance of guest breakpoint recovery or a production runtime change.


The normal `build-fex.sh` now applies the callback guard and single-step return
fix, builds both PE modules and Unix libraries, and writes `xrgame-fex-build.json`.
`package-components.py` checks its pin, patch hashes and four binary hashes before
packaging FEX xrg5. The context-trace helper above deliberately rejects that newer
cached source: it reproduces the historical failure only. Its `--mode work-guard`
plus `--extra-patch`/`--output` options were used for isolated fix trials before
promotion. No child gitlink or upstream repository was changed.

### Foundation DebugBus (internal debug APK)

Initialize the pinned root `foundation/` submodule with authorized private-repository access.
Only `modules/debugbus` is linked. Bring XRGame to the foreground, then run
`python3 tools/xrgame/debugbus.py --serial <device> --start bridge`.
See [commands and host/guest boundaries](../../docs/debugging/debugbus.md).
`modules libxrgame_debugbus.so` reports the live host ELF Build ID; `present trace 120`
collects a bounded host Present timeline without restarting the game.

### Read existing ntsync counters without restarting Wine

`read-ntsync-counters.c` reads the 15 process-local AtomicU64 counters in pinned
ntsync-android `7ce6435e5979b1cb5341aa4b299f31e8937fe121`. Build for Android arm64
with NDK clang (API 26 or later), `-O2 -Wall -Wextra -Werror`. Run under the app UID:

```text
/system/bin/linker64 <reader> <PID> <start-time-ticks> <COUNTERS-address> <seconds> <interval-ms>
```

Resolve `stats::COUNTERS` in the exact mapped `ntdll.so` using its ELF symbol table,
PT_LOAD segments and current process maps; verify the file SHA and process generation.
Never reuse a previous process address or assume this private layout is a stable ABI.
The reader opens `/proc/PID/mem` read-only, checks start-time every sample, and brackets
each 120-byte read with CLOCK_MONOTONIC timestamps. Limits are 1–120 seconds and
100–2000 ms between samples. Cross-field snapshots are not atomic. Permission or
generation failures terminate the capture; use `adb shell` when checking the remote exit code.

Keep raw output outside the repository. Align deltas with Present timestamps before
normalizing by frame requests. `futex_sleeps` counts attempts, not actual sleeps;
`lock_contended=0` does not exclude mutex waiting, and wake latency is unavailable
without separate instrumentation. Do not enable NTSYNC_DEBUG or disable SIGUSR1
protection merely to read these existing counters. See the
[device results and exact counter semantics](../../docs/validation/mhw-sync-20260927.md).

### Sample the objects behind a blocked ntsync wait

`sample-ntsync-waits.c` is a bounded, read-only Android arm64 diagnostic for the
same pinned ntsync v9 layout. Build with NDK clang (API 28),
`-O2 -Wall -Wextra -Werror`. Verify the loaded ntdll SHA and current PID/start
ticks first; resolve the session's v9 shared mapping from that PID's maps:

```text
/system/bin/linker64 <reader> <PID> <start-ticks> <shared-mapping-base> <seconds> <interval-ms> <TID> [TID ...]
```

It checks each thread generation, reads the kernel's current syscall, resolves
shared futex waiter/slot identities, and repeats syscall/node/slot reads to
reject observed races. It never locks or writes the shared region. `stable`
is best-effort consistency, not an atomic snapshot. Samples measure occupancy,
not call counts, exact sleep duration, or a complete object lifecycle. Keep
`stable:false` rows and exclude them from object attribution. Slot generations
and addresses cannot be reused across process/session identities.

Limits: 1–60 seconds, 5–1000 ms intervals, at most 8 explicitly selected threads.
Prefer one thread at 100 ms; short 10–20 ms diagnostic windows cost more. On AYN,
4 threads at 20 ms cost 436.166 ms observer CPU over 10 seconds (4.36% of one core),
excluding ADB/host work; this is not a measured game FPS overhead. The final
observer elapsed/CPU summary goes to stderr, data rows to stdout. Exit codes:
2 invalid arguments, 3 memory access denied, 4 stale/missing generation,
5 unsupported/incorrect region, 6 allocation failure, 7 syscall observation failed.
There is no resident service and no profiler requirement. See the
[AI LIMIT object and wake-duration evidence](../../docs/validation/ai-limit-sync-fastpath-20261002.md).


### Wine media startup checks

`build-gst-libav.sh` builds the pinned owned GStreamer fork against the existing
1.28.0 / FFmpeg 8.0.1 snapshot. `package-imagefs.py` includes the non-GL Wine
playback elements and all their mandatory ELF dependencies. Optional Wine dlopen
roots do not exempt a transitive DT_NEEDED library. Android `libmediandk.so` must
come from the system; the old Termux substitute breaks system-library version
resolution on AYN. The image record identifies the exact obsolete file hash.

`XrGameBaseImage` updates existing installations from a verified bundle without
clearing homes or prefixes. Its receipt is written only after all recorded files
are installed and known obsolete files are removed. Unrecognized modified files
are preserved and reported. Source/publication acceptance remains separate.

Build `probe-media-runtime.c` with NDK clang (`-O2 -Wall -Wextra -Werror -ldl`),
then run it through `/system/bin/linker64` under the app UID, with the runtime's
`LD_LIBRARY_PATH`, `GST_PLUGIN_PATH`, and `GST_REGISTRY_FORK=no`. Pass the absolute
`winegstreamer.so` and `libgstlibav.so` paths. Exit 0 confirms both libraries load
and 11 required element factories exist; it does not establish video playback.
See `docs/validation/mhr-startup-20260927.md` for failed trials and game evidence.
