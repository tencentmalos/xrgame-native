# references/

Independently versioned source checkouts used for comparison, porting and pinned builds. JavaSteam supplies the app's source-built protocol dependency; `tools/xrgame/` builds pinned runtime components from the documented Wine/FEX/Proton/Mesa sources. Other ports into `app/` retain their origin and license in `THIRD_PARTY_NOTICES`.

The authoritative revision of each checkout is the gitlink recorded by this repository, not the tip of the tracked branch. Do not run `git submodule update --remote` as an incidental setup step.

For XRGame's embedded Proton 11 default, FEX maintenance starts from the same `tencentmalos/FEX` commit used by shadPS4: `3f1f30a060b633980ed8e7674eb8d8997457edad`. The published maintenance branch is `feature/malos/xrgame-arm64ec`; its gitlink remains at that shared baseline. Runtime-specific FEX patches are recorded in `tools/xrgame/patches`. Keep AI-authored changes in the owned fork, not upstream FEX.

| Path | Maintenance repository | Tracked branch | Pinned at | Purpose | License |
|---|---|---|---|---|---|
| `proton` | [tencentmalos/Proton](https://github.com/tencentmalos/Proton) | `proton_11.0` | `5b89db94` (proton-11.0-2c, 2026-09-04) | Official Proton 11 build system. Reference for the `arm64ec-windows` ARCH, the wine/dxvk/vkd3d-proton/FEX/wineopenxr rules (`Makefile.in`), lsteamclient and steam_helper. Its own submodules are **not** initialized. | BSD-3-Clause (Proton's own code); submodules carry their own licenses |
| `proton-wine` | [tencentmalos/proton-wine](https://github.com/tencentmalos/proton-wine) | `feature/malos/wine-guest-debug` | `a0200a75` (2026-09-28 checkpoint) | The bionic arm64ec / x86_64 Proton Wine that GameNative actually ships. See `.github/workflows/build-proton.yml` and `build-scripts/`. Shallow. | LGPL-2.1+ (Wine) |
| `FEX` | [tencentmalos/FEX](https://github.com/tencentmalos/FEX) (fork of FEX-Emu/FEX) | `feature/malos/xrgame-arm64ec` | `3f1f30a0` (FEX-2608-241) | Same pin as shadPS4. `Source/Windows/{ARM64EC,WOW64,UnixLib}` are the Wine-hosted emulator modules. **FEX's own AGENTS.md/CLAUDE.md forbid AI-generated contributions upstream.** | MIT |
| `shadPS4` | [tencentmalos/Bachata-S4](https://github.com/tencentmalos/Bachata-S4) (shadPS4 Android/FEX port) | `malos/main` | `a562e810` (2026-09-24) | Android host components to port: session lifecycle, Vulkan presenter, Turnip loading, Oboe audio, input, diagnostics, Litep/KGSL tooling and validation methodology. Its own submodules are **not** initialized. | GPL-2.0-or-later per SPDX headers (compatible with this repo's GPL-3.0); its private Foundation dependency is separately authorized for internal debug builds (2026-09-27), pinned at root `foundation/`; see `docs/debugging/debugbus.md` |
| `mesa-turnip` | [tencentmalos/mesa-mirror](https://github.com/tencentmalos/mesa-mirror) | `feature/malos/xrgame-wine-icd` | `22451ea2` (2026-10-09, Turnip xrg10 source) | Turnip fork used by shadPS4 on Adreno (KGSL zero-timeout poll, gralloc/Mapper metadata, fragment density map 2 for XR). Shallow. | MIT |
| `WinNative` | [tencentmalos/WinNative](https://github.com/tencentmalos/WinNative) | default | `e9e5d307` (2026-09-23) | Comparison frontend: Vulkan compositor, FEX UnixLibs toggle, and its own Rust Steam client `wnsteam` (`app/src/main/cpp/wn-steam-client/rust`: CM client, auth, depot download; no JVM). Shallow. | GPL-3.0 |
| `gfxreconstruct` | [tencentmalos/gfxreconstruct](https://github.com/tencentmalos/gfxreconstruct) (fork of LunarG/gfxreconstruct) | `feature/malos/xrgame-wine-capture` | `3868cd12` (LunarG `dev` `6dc9b65` + Wine capture, 2026-10-05) | API capture and replay for picoXr debug APKs: D3D12/DXGI capture proxies, the Android Vulkan capture layer and the Windows replay tools. See [the API replay spec](../docs/specs/xrgame-native-api-replay-v1.md). Shallow. Its own submodules are **not** initialized. | MIT |

### Steam client implementations (spec WP1 / WP6)

| Path | Maintenance repository | Tracked branch | Pinned at | Purpose | License |
|---|---|---|---|---|---|
| `JavaSteam` | [tencentmalos/JavaSteam](https://github.com/tencentmalos/JavaSteam) (fork of Longi94/JavaSteam) | `gamenative-latest` | `433f2ad1` ("bump version 1.8.0.1-26-SNAPSHOT") | The exact source of the `io.github.joshuatam:javasteam(-depotdownloader) 1.8.0.1-26-SNAPSHOT` the app depends on. WP1 builds it from here to drop the SNAPSHOT dependency. | MIT |
| `Pluvia` | [tencentmalos/Pluvia](https://github.com/tencentmalos/Pluvia) | `master` | `467a631f` (2026-02-23) | The original Android Steam client on JavaSteam that GameNative's `SteamService` descends from; a minimal reference for login/library/download. | GPL-3.0 |
| `SteamKit` | [tencentmalos/SteamKit](https://github.com/tencentmalos/SteamKit) | `master` | `84c990c3` (2026-09-15) | SteamKit2 (.NET), the protocol implementation JavaSteam ports. Shallow. | LGPL-2.1 |
| `DepotDownloader` | [tencentmalos/DepotDownloader](https://github.com/tencentmalos/DepotDownloader) | `master` | `989f37b1` (2026-09-21) | Reference depot-download semantics (manifests, depot keys, chunk verification); used on a PC to cross-check files installed by the app. | GPL-2.0 |
| `gbe_fork` | [tencentmalos/gbe_fork](https://github.com/tencentmalos/gbe_fork) | `feature/malos/xrgame-loader` | `c1ca5f8f` (2026-09-28 checkpoint) | Goldberg-emulator fork whose `steam_api(64).dll` builds the app bundles for the emulated-DRM launch mode. Shallow. | LGPL-3.0 |

## Setup

```bash
git submodule update --init references/proton references/FEX references/shadPS4 \
    references/JavaSteam references/Pluvia references/DepotDownloader
git submodule update --init --depth 1 references/proton-wine references/mesa-turnip references/WinNative \
    references/SteamKit references/gbe_fork references/gfxreconstruct
```

On Windows, mesa needs long paths: `git -C references/mesa-turnip config core.longpaths true`.

Turnip maintenance policy (2026-09-26): use `malos/main` in our fork. Its initial
baseline is shadPS4's `references/mesa-turnip-xr-fdm2`, branch
`codex/turnip-xr-fdm2`, commit `d15b7c019c8daa17e80051258077d9b2d5146a2b`.
Both `malos/main` and `feature/malos/xrgame-wine-icd` are now published. The
feature branch carries the Android HAL/Wine ICD exports and X11 AHB presentation.
The parent gitlink records that source checkpoint. Since 2026-10-09 it is `22451ea2`:
the shadPS4 and Azahar Swan XR Turnip merge (`25ef1647`), the AHB entry points
(`51f94d33`), and the LRZ RMW / concurrent binning hang fix. Its tree is exactly what
`turnip-25ef164-xrg10` was built from. The same fix sits alone on `malos/main` in
`feature/malos/lrz-rmw-no-cb` (`16ad8431`); `malos/main` itself is unchanged. Validated runtime recipes keep
their explicit older base commits and recorded build-copy patches; advancing a
maintenance gitlink does not silently change the bundled binaries. Wine and GBE
follow the same distinction. See the [stage record](../docs/validation/stage-20260928.md).

Local mirrors on the original workstation (optional `--reference` sources): `C:\workspace\proton11`, `C:\workspace\emulations\shadps4`.

## Fork maintenance (2026-09-26)

At the original fork migration, all 13 direct submodules were moved to `tencentmalos` without changing their gitlinks. The 2026-09-28 checkpoint advances the maintenance gitlinks shown above.
Existing upstream remotes were retained for initialized checkouts. GBE uses our
`gbe_fork` fork at `7a319f0b`; its relative third-party submodules resolve within
the same fork, which includes all upstream branches.

The runtime recipes also use owned forks for `ntsync-android`, `dxvk`,
`vkd3d-proton`, and `pulseaudio-android`, retaining their exact source pins.
`termux-on-gha` and `llvm-mingw` have source forks as well; the existing SHA-pinned
build-tool archives still come from upstream releases because GitHub forks do
not copy release assets. This is separate from adopting their source for future
maintenance. Nested third-party dependency URLs remain upstream and pinned.

The media recipe uses `tencentmalos/gstreamer`, forked from `GStreamer/gstreamer`,
at the 1.28.0 commit `9058212f43074ef7df229e73cea135c4ea96e0d6`. Its checkout
is under the Linux build root's `src/gstreamer`; the recipe pin is authoritative
for this build-only dependency. `build-gst-libav.sh` builds the plugin against
the existing GStreamer/FFmpeg snapshot, with the optional libavfilter-backed
filters omitted in a build copy. The source archive collector includes this pin.

## OpenComposite (2026-10-02)

`references/opencomposite` is `tencentmalos/opencomposite`, forked from
`GameNative/opencomposite`, initially at `7fd3276ac0ffda518a7f5a9157620f292e2032e6`.
The maintained gitlink is `7f87df1fe7e6c71b6af38cbba5c39b90ee6130e6` (2026-10-09: adds the
mailbox forwarding patch for Alyx loading interstitials).
It contains the GPL-3.0 build recipe and patch, not a vendored full OpenOVR tree.
The recipe pins `https://gitlab.com/znixian/OpenOVR` at
`a27e7e6a64bdcd1eff6b7fba1ea2ea34bcf1273d`. Nested upstream pins remain unchanged.
The picoXr validation payload now uses a local Windows/MSVC source build with the
OpenVR 2.15.6 ABI patch from `feature/malos/xrgame-sbs-openvr26`. The exact DLL,
patch/header checksums, compiler, nested pins and build inputs are recorded in
[`tools/xrgame/opencomposite-pin.json`](../tools/xrgame/opencomposite-pin.json).
Stage the matching DLL at `build/xrgame-runtime/opencomposite/opencomposite_x64.dll`;
packaging fails if its hash or the recipe sources differ. The upstream flavor's
reviewed v2 asset is unchanged as a rollback baseline.

The unmodified OpenVR 2.15.6 header is pinned at Valve
`0924064316de3effbcd1acf1e309182a2deb1c05` (BSD-3-Clause), mirrored in
`tencentmalos/openvr`. Full MSVC compilation/linking and generated ABI checks pass.
The Windows host uses Vulkan headers `ee3b5caaa7e372715873c7b9c390ee1c3ca5db25`
from `tencentmalos/Vulkan-Headers` and an x64 import library made from
`tencentmalos/Vulkan-Loader` `466498bc64eb77955c3b782f0127520548224de0`'s
`loader/vulkan-1.def`. These are source build inputs, not a proprietary GPU driver.
See [SBS validation](../docs/validation/vr-sbs-20261002.md) for the narrower device
results and remaining gameplay gates. Only source is published; the internal
validation payload has no public binary release.

2026-10-02：GBE 在 `feature/malos/xrgame-local-sockets` 保留离线进程内消息队列与回调；
维护 gitlink 为 `e6dab9c5a70dd0aefd29e704b4ee557921874569`。
`disable_networking=1` 继续禁止远端网络/广播。验证客户端仍从原
`7a319f0bedad260f952b0fb367b27f255fd952c5` 加主仓
`tools/xrgame/patches/gbe-offline-local-ipc.patch` 重建，不把较新维护 gitlink 自动当作
已验证 runtime。Windows 对照探针及 Alyx SBS 主菜单证据见 VR 验证记录。

## GFXReconstruct (2026-10-05)

`references/gfxreconstruct` is `tencentmalos/gfxreconstruct`, forked from
`LunarG/gfxreconstruct` at `dev` `6dc9b65a03734070b1c1d3809e483e52dd8cefc9`.
`feature/malos/xrgame-wine-capture` adds one commit,
`3868cd12ebe01cb06328bd7526519a97997961b4`. Its diff is byte-identical to
`tools/xrgame/patches/gfxreconstruct-wine-capture.patch`. Upstream pull requests
require LunarG's CLA (see its `CONTRIBUTING.md`).

Building needs its `external/` submodules (Vulkan-Headers, SPIRV-Headers, SPIRV-Reflect,
OpenXR-SDK and OpenXR-Docs). Initialize them with
`git -C references/gfxreconstruct submodule update --init --depth 1`.
The 2026-10-04 binaries were built on Windows 11, and every configure step passed
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`:

- **Capture DLLs** (`d3d12.dll`, `dxgi.dll`, `d3d12_capture.dll`): Visual Studio 2022 x64
  with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`.
  - Build with `CL=/utf-8`; under the GBK code page, warning C4819 is treated as an error.
- **Replay tools** (`gfxrecon-replay`, `gfxrecon-info`, `gfxrecon-convert`): a default `/MD` build.
  - A static CRT conflicts with the OpenXR loader.
  - Keep the `D3D12\` folder from the build output, which holds the Agility SDK `D3D12Core.dll`, beside `gfxrecon-replay.exe`.
- **Android layer**: `android/layer` with NDK 27.3 (`ANDROID_ABI=arm64-v8a`,
  `ANDROID_PLATFORM=26`, `ANDROID_STL=c++_static`, `-DGFXRECON_ENABLE_OPENXR=OFF`), then stripped.

`tools/xrgame/stage-gfxr.py` stages the capture DLLs and the layer for internal picoXr debug APKs only.
