# Guest patches (XRGame)

Per-game packages of x86-64 code that SteamPSP applies to a Windows game's own modules
(the EXE or its DLLs) before that code runs, without changing game files. This is a port of
shadPS4's guest function patches to Windows x64 games under Wine/FEX. The design is in
[the spec](../../../docs/specs/xrgame-guest-patch-v1.md).

| Path | What |
|---|---|
| `build.py` | Recipe (`xrgame.guest-patch.recipe.v1`) → package `patch.json` (`xrgame.guest-patch.v1`). |
| `relocate.py` | Function-entry trampoline templates (capstone), computed at build time. |
| `sdk/` | Payload SDK: `xrg_guest.h` (services, Windows x64 calls), `xrg_site.h`, site adapter, linker script. |
| `test-probe.py` | Native end-to-end check on a Windows x64 PC with the probe fixture (`../fixtures/guest-patch-probe`). |
| `games/<appid>/` | Recipes for real games. 546560 (Half-Life: Alyx): two chain probes and the ETFR packages `etfr_fdm_split`, `etfr_offset_eye`, `etfr_fdm_ab` ([record](../../../docs/validation/swan-alyx-etfr-guest-patch-20261016.md)). |
| `../../../app/src/main/windows/guest_patch/` | The installer DLL injected into the game. |

## Build a package

Needs an LLVM toolchain with `clang`, `ld.lld` and `llvm-objdump` (the Android NDK's is fine)
and Python with `capstone`.

```sh
python3 tools/xrgame/guest-patch/build.py \
  --clang "$ANDROID_NDK/toolchains/llvm/prebuilt/<host>/bin/clang" \
  --recipe tools/xrgame/guest-patch/games/546560/exe_entry_probe.recipe.json \
  --output build/guest-patch/exe_entry_probe
```

A recipe names the game (`title` = Steam app id), the target `module` (PE file name) and its
`module_sha256`. It then lists:

- `hooks`: function-entry replacements. Each has a C prototype for the original (called with
  the Windows x64 convention), the entry `offset`, and `expected` bytes covering at least 5 bytes
  of whole instructions.
- `sites`: `before` or `replace` handlers at an instruction boundary, which get the integer and
  SSE state (`XrgSiteContext`).
- `patches`: same-length byte edits, with the original bytes checked first.
- `bindings`: game functions and data the payload uses.
- `counters` and `logs`: declared ids for `xrg_sdk_counter` and `xrg_sdk_log`.
- `defines`: preprocessor names for the sources.

Every entry needs `evidence`. Payload rules:

- Payloads are freestanding: no CRT, no exceptions or RTTI, no TLS, no constructors.
- Keep stack frames well below 4 KiB.
- An exception must never unwind through a hook frame, because payload code has no Windows
  unwind data.

## Install and select on the device

```sh
adb push build/guest-patch/exe_entry_probe/patch.json \
  /sdcard/XRGameNative/guest_patches/546560/exe_entry_probe.json
adb shell dumpsys activity service com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService \
  guest_patch select 546560 exe_entry_probe,client_dllmain_probe
```

You can also turn packages on in the launch panel: each package is one "Patch: <name>" row,
and packages that conflict cannot both be on. The selection takes effect at the next launch.

`guest_patch status [appid]` shows the packages, the selection and the installer's status:
installed or not with the reason, hooks with their addresses, and counter samples.
`guest_patch enable|disable [package|hook|package/hook]` switches hooks of the running game.
The installer logs to `C:\xrgame\guest_patch\<appid>\guest-patch.log` in the prefix.

## Check the chain on a PC

```sh
python tools/xrgame/guest-patch/test-probe.py --llvm-bin <ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin --output build/guest-patch-probe
```

The check builds the installer and the probe fixture and generates the fixture's recipe. It
then runs the fixture unpatched, and again with the installer loaded while one hook is switched
off midway. It covers typed hooks (including trampolines with a call and a RIP-relative load),
both site modes, a code patch, function and data bindings, counters, logs and the control file.
