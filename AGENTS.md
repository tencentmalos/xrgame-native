# Repository context for coding agents

## Project objective and current state

- `xrgame-native` is a fork of [GameNative](https://github.com/utkarshdalal/GameNative) (GPL-3.0). It targets **Windows / Steam games on a Pico XR headset** (internal device "Swan": Android 16, ARM64, 4 KiB pages, Adreno 840v2 / KGSL): a 2D theater mode plus PCVR through an OpenXR bridge.
- The current requirements are in [docs/specs/xrgame-native-v1.md](docs/specs/xrgame-native-v1.md). The route analysis behind it is in [docs/background/](docs/background/).
- **State (2026-09-24): fork created, references added, spec v1.2 drafted with the §9 decisions recorded. No v1 work package has been implemented.** The app is still upstream GameNative @ `ebde76e9`. Specifications are requirements, not evidence of implementation.
- **Order of work:**
  - WP0: repo governance plus minimal identity (applicationId/name/signing).
  - **WP1: install Steam games first.** Build JavaSteam from `references/JavaSteam` to drop the SNAPSHOT dependency, and `libgndownload` from its in-repo Rust source. Use an external-storage install root, support directory import, and cross-check files against DepotDownloader.
  - Every later device acceptance uses games installed by WP1.

## Repository layout

- `app/`: upstream GameNative (Kotlin/Java UI and services, the Winlator-derived `com.winlator` runtime, native code under `app/src/main/cpp`, and the Windows-side XR runtime under `app/src/main/windows`).
- `references/`: independently versioned checkouts for comparison and porting, never built into the app. See [references/README.md](references/README.md) for purposes, pins and licenses.
- `docs/specs/`: normative specs. `docs/background/`: route evaluations copied from the shadPS4 workspace. `docs/validation/`: evidence records (create per work package).
- `tools/`: upstream build helpers. The XR payload scripts are PowerShell and need NDK 29.0.14206865 + VS2022.

## Branches and upstream

- `malos/main` is the integration branch. Feature work goes on `feature/malos/<topic>`, based on the latest `malos/main` unless the user says otherwise.
- `origin` = `tencentmalos/xrgame-native`; `upstream` = `utkarshdalal/GameNative`. Upstream moves daily. Merge `upstream/master` into `malos/main` periodically and record conflict hotspots.
- Keep our changes concentrated in the new `picoXr` flavor's source set and in build-time switches. Do not delete or rewrite upstream code in `src/main` just to disable it.
- Commit or push only when the user asks.

## Hard constraints (spec §3)

1. **Own identity.** applicationId `com.tencentmalos.xrgamenative`, app name "XRGame Native", with its own icon and signing; never ship as "GameNative". Never use the PKCS#12 `keystore` committed at the repo root.
   - APKs are distributed to internal devices only. This repo is public, so CI must not upload APKs as Actions artifacts or Release assets. Only runtime components go to this repo's GitHub Releases, each with its license and source pointer (spec C4, §9).
2. **No package-locked or source-less binaries on the v1 path.** This covers `libredirect*`, `libkgslshim`, the steamhost/eahost/rgschost stubs, `libsteambootstrap`, `libvulkan_wrapper` and `libvortekrenderer`. Replace them or keep them off the path.
   - The exec/W^X shim that replaces `libredirect` must be a **clean-room** implementation. Do not disassemble and port the proprietary binary.
3. **No upstream services.** Do not use the manifest from upstream master, `downloads.gamenative.app` or its R2 bucket, `api` / `relay.gamenative.app`, the updater or Play Integrity. PostHog stays off with an empty key.
4. **License hygiene.**
   - Do not distribute the committed Qualcomm proprietary drivers.
   - Do not enable Steamless.
   - Keep `THIRD_PARTY_NOTICES` complete.
   - Code ported from `references/shadPS4` keeps its `GPL-2.0-or-later` SPDX header.
   - shadPS4's private Foundation library must never enter this repo.
5. **Evidence.**
   - Every acceptance record includes: APK SHA-256, `.so` Build IDs, component manifest SHA, and device identity (model, build, boot id), PID and duration.
   - Keep failed and aborted captures.
   - Never state numbers that were not measured.
   - Never commit game data, APKs, keystores or credentials.

## References and dependencies

- The gitlink is the authoritative revision. Do not run `git submodule update --remote` incidentally.
- `references/FEX` is `tencentmalos/FEX`, the same pin as shadPS4. **FEX's own AGENTS.md / CLAUDE.md forbid AI-generated contributions upstream.** Changes stay in the tencentmalos fork, and upstreaming is done by humans.
- `references/mesa-turnip` is `tencentmalos/mesa-mirror` `codex/turnip-xr-fdm2` (the Turnip used by shadPS4 on Swan). Push child-repo changes to their owned branch before advancing a parent gitlink.
- `references/proton-wine` is the bionic arm64ec Proton Wine that GameNative ships; its `.github/workflows/build-proton.yml` is the build recipe (x86_64 runner, NDK r27d, bylaws llvm-mingw).
- Steam client references are `JavaSteam`, `Pluvia`, `SteamKit`, `DepotDownloader` and `gbe_fork`. WinNative's Rust `wnsteam` client is inside `references/WinNative`.
  - Valve's CM servers and CDN are the only allowed remote endpoints in the install flow.
  - Test only with the dedicated Steam test account the user provides, never a personal main account. The user types the credentials on the device; never put account names, passwords, tokens or Steam Guard data in the repo or in validation records.
- The upstream GameNative app pins submodules `app/src/main/cpp/extras/adrenotools` and `app/src/main/cpp/lsfg-vk-android`.

## Building

- JDK 17, Android SDK platform 36, Gradle wrapper 8.12.1, about 8 GB heap. JavaSteam is a SNAPSHOT dependency, so builds need network access and are not reproducible yet (spec WP1).
- Debug builds of the upstream flavors: `./gradlew :app:assembleLegacyDebug`, `assembleModernDebug`, `assembleLegacyXrDebug`, `assembleModernXrDebug`. The `picoXr` flavor does not exist yet (created in spec WP0, XR-adapted in WP5).
- Every `externalNativeBuild` block in `app/build.gradle.kts` is commented out. Native libraries ship as prebuilt `.so` files in `jniLibs`. Restoring source builds is spec WP3 (`libgndownload` comes first, in WP1).

## Devices and host

- Main acceptance device: **Swan** (Pico headset, Android 16, 4 KiB, Adreno 840v2, adb root available). Auxiliary: **AYN Thor** (Android handheld, non-XR smoke tests only). Serial numbers and root tooling are recorded in the local agent memory, not in this public repo.
- The workstation is Windows 11. Git Bash heredocs mangle backslashes, and the working copy uses CRLF (`core.autocrlf`). For mesa, set `core.longpaths=true`.
