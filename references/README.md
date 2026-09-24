# references/

Independently versioned source checkouts used for comparison, porting and reference. **Nothing under `references/` is built into the app.** Code is copied or ported into `app/` deliberately, with its origin and license recorded in the commit and in `THIRD_PARTY_NOTICES`.

The authoritative revision of each checkout is the gitlink recorded by this repository, not the tip of the tracked branch. Do not run `git submodule update --remote` as an incidental setup step.

| Path | Upstream | Tracked branch | Pinned at | Purpose | License |
|---|---|---|---|---|---|
| `proton` | [ValveSoftware/Proton](https://github.com/ValveSoftware/Proton) | `proton_11.0` | `5b89db94` (proton-11.0-2c, 2026-09-04) | Official Proton 11 build system. Reference for the `arm64ec-windows` ARCH, the wine/dxvk/vkd3d-proton/FEX/wineopenxr rules (`Makefile.in`), lsteamclient and steam_helper. Its own submodules are **not** initialized. | BSD-3-Clause (Proton's own code); submodules carry their own licenses |
| `proton-wine` | [GameNative/proton-wine](https://github.com/GameNative/proton-wine) | `proton_11.0-2` | `5d0d333e` (proton-11.0-2-20260918) | The bionic arm64ec / x86_64 Proton Wine that GameNative actually ships. See `.github/workflows/build-proton.yml` and `build-scripts/`. Shallow. | LGPL-2.1+ (Wine) |
| `FEX` | [tencentmalos/FEX](https://github.com/tencentmalos/FEX) (fork of FEX-Emu/FEX) | `feature/malos/host-page-size` | `3f1f30a0` (FEX-2608-241) | Same pin as shadPS4. `Source/Windows/{ARM64EC,WOW64,UnixLib}` are the Wine-hosted emulator modules. **FEX's own AGENTS.md/CLAUDE.md forbid AI-generated contributions upstream.** | MIT |
| `shadPS4` | [tencentmalos/Bachata-S4](https://github.com/tencentmalos/Bachata-S4) (shadPS4 Android/FEX port) | `malos/main` | `a562e810` (2026-09-24) | Android host components to port: session lifecycle, Vulkan presenter, Turnip loading, Oboe audio, input, diagnostics, Litep/KGSL tooling and validation methodology. Its own submodules are **not** initialized. | GPL-2.0-or-later per SPDX headers (compatible with this repo's GPL-3.0); its private Foundation dependency is **not** covered, see the spec |
| `mesa-turnip` | [tencentmalos/mesa-mirror](https://github.com/tencentmalos/mesa-mirror) | `codex/turnip-xr-fdm2` | `d15b7c01` (2026-09-23) | Turnip fork used by shadPS4 on Adreno (KGSL zero-timeout poll, gralloc/Mapper metadata, fragment density map 2 for XR). Shallow. | MIT |
| `WinNative` | [WinNative-Emu/WinNative](https://github.com/WinNative-Emu/WinNative) | default | `e9e5d307` (2026-09-23) | Comparison frontend: Vulkan compositor, FEX UnixLibs toggle, and its own Rust Steam client `wnsteam` (`app/src/main/cpp/wn-steam-client/rust`: CM client, auth, depot download; no JVM). Shallow. | GPL-3.0 |

### Steam client implementations (spec WP1 / WP6)

| Path | Upstream | Tracked branch | Pinned at | Purpose | License |
|---|---|---|---|---|---|
| `JavaSteam` | [joshuatam/JavaSteam](https://github.com/joshuatam/JavaSteam) (fork of Longi94/JavaSteam) | `gamenative-latest` | `433f2ad1` ("bump version 1.8.0.1-26-SNAPSHOT") | The exact source of the `io.github.joshuatam:javasteam(-depotdownloader) 1.8.0.1-26-SNAPSHOT` the app depends on. WP1 builds it from here to drop the SNAPSHOT dependency. | MIT |
| `Pluvia` | [oxters168/Pluvia](https://github.com/oxters168/Pluvia) | `master` | `467a631f` (2026-02-23) | The original Android Steam client on JavaSteam that GameNative's `SteamService` descends from; a minimal reference for login/library/download. | GPL-3.0 |
| `SteamKit` | [SteamRE/SteamKit](https://github.com/SteamRE/SteamKit) | `master` | `84c990c3` (2026-09-15) | SteamKit2 (.NET), the protocol implementation JavaSteam ports. Shallow. | LGPL-2.1 |
| `DepotDownloader` | [SteamRE/DepotDownloader](https://github.com/SteamRE/DepotDownloader) | `master` | `989f37b1` (2026-09-21) | Reference depot-download semantics (manifests, depot keys, chunk verification); used on a PC to cross-check files installed by the app. | GPL-2.0 |
| `gbe_fork` | [Detanup01/gbe_fork](https://github.com/Detanup01/gbe_fork) | `dev` | `7a319f0b` (2026-09-21) | Goldberg-emulator fork whose `steam_api(64).dll` builds the app bundles for the emulated-DRM launch mode. Shallow. | LGPL-3.0 |

## Setup

```bash
git submodule update --init references/proton references/FEX references/shadPS4 \
    references/JavaSteam references/Pluvia references/DepotDownloader
git submodule update --init --depth 1 references/proton-wine references/mesa-turnip references/WinNative \
    references/SteamKit references/gbe_fork
```

On Windows, mesa needs long paths: `git -C references/mesa-turnip config core.longpaths true`.

Local mirrors on the original workstation (optional `--reference` sources): `C:\workspace\proton11`, `C:\workspace\emulations\shadps4`.
