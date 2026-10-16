# Guest patch v1（Windows x64 游戏）

2026-10-15，按用户要求参考 shadPS4 的 guest function patch 实现。

## 目的

给某个游戏的某个确定版本打代码补丁，不修改游戏文件：

- 函数入口替换（C/C++）；
- 任意指令边界的处理函数（site）；
- 同长度字节补丁；
- 计数器和日志。

补丁在目标模块的代码运行之前装好；同一局里可以按 hook 开关。

这是 shadPS4 guest patch 的移植（用户的 shadPS4 fork `docs/guest-function-patches.md`，
`src/core/host_runtime/guest_patch_format.cpp`、`guest_patch_relocator.cpp`、
`src/core/guest_patch_desktop.cpp`）。差异：

- 游戏是 Wine 下的 PE x64 程序，FEX ARM64EC 执行其中的 x64 代码。
- 包面向游戏的函数使用 Windows x64 调用约定。

## 链路

1. **打包**（PC，`tools/xrgame/guest-patch/build.py`）：
   - recipe 里的 C/C++/汇编编译成 freestanding x86-64 ELF（`--target=x86_64-none-elf`，SSE2，无 CRT）。
   - 面向游戏的原型（hook 的 replacement/original、函数绑定、SDK 服务）加 `ms_abi`。
   - 入口被搬走的指令由 `relocate.py`（capstone，移植 shadPS4 的 Zydis 重定位器）在打包时解码，
     生成「代码 + fixup」模板。游戏内不需要反汇编器。
   - 输出 `patch.json`，schema `xrgame.guest-patch.v1`，abi `x86_64-win64`。
2. **选择**（app）：
   - 包放在 `/sdcard/XRGameNative/guest_patches/<Steam appid>/<名字>.json`。
   - 每个游戏的选择存在容器变量 `XRGAME_GUEST_PATCHES`（按安装顺序的文件名，最多 8 个）。
   - 启动面板每个包一行开关：
     - 改到同一段代码（或调用了对方改过的函数）的包不能同时打开；
     - DebugBus `guest_patch select` 也能设置选择。
3. **启动**（`XrGameGuestPatches.prepare`，`XServerScreen.getWineStartCommand`）：
   - 把选中的包按序复制到前缀 `C:\xrgame\guest_patch\<appid>\packages\NN-名字.json`；
   - 从 APK 资产取出 `xrgame_guest_patch64.dll`，按 `xrgame-payload.json` 校验 SHA；
   - 设置 `XRGAME_GUEST_PATCH_DIR` 和 `XRGAME_GUEST_PATCH_APP`。
   - 内置 Steam 客户端加载器（GBE ColdClientLoader）的 `DllsToInjectFolder` 指向每局重建的
     `C:\xrgame\inject\<appid>`：客户端额外 DLL（若有）在前，安装器在后，由 `load_order.txt` 指定。
   - 加载器先以挂起方式创建游戏进程，然后远程 LoadLibrary 注入，最后才恢复主线程。
4. **安装**（`app/src/main/windows/guest_patch/xrgame_guest_patch.c`，x64 PE、无 CRT，只导入 kernel32/ntdll/bcrypt）：
   - 解析并校验包（移植 shadPS4 的 `Package::Load` 检查）。
   - 已加载的模块（主 EXE、静态导入的 DLL）在 DllMain 里立即安装；之后加载的 DLL 由
     `LdrRegisterDllNotification` 在其 DllMain 之前安装。
   - 安装前的检查（移植 BuildPlan + 桌面加载器）：
     - 模块文件 SHA-256 一致（bcrypt；按 路径/大小/时间 缓存到 `sha-cache.txt`）；
     - 原始字节一致，并落在可执行节内；
     - 没有和已装的包冲突。
   - 写入顺序（移植 BuildPlan + 桌面加载器）：
     1. 在模块 ±2 GiB 内分配内存，放 payload 和 pool。pool 里每个 hook 一个 stub：
        `jmp [slot]` 加一段原函数跳板；后面跟一页 dispatch slot。
     2. 填 import slot：SDK 服务、hook 的续接、绑定。
     3. payload 代码和 stub 改为 RX，数据和 slot 保持 RW。
     4. 最后用 `VirtualProtect` + `FlushInstructionCache` 写入入口的 `jmp rel32` 和字节补丁。
        FEX ARM64EC 在这两种通知上失效已编译的代码（`NotifyMemoryProtect`、
        `BTCpu64FlushInstructionCache`）。
   - 不匹配或冲突的包跳过，原因写进状态，游戏照常运行。
5. **观察与开关**：
   - 安装器在 `C:\xrgame\guest_patch\<appid>\` 下写这几个文件：
     - `status.txt`：包、hook 地址、是否启用、计数器；
     - `guest-patch.log`：安装事件和 `xrg_sdk_log`；
     - 轮询 `control.txt`（每 250 ms）：`<序号> enable|disable [包|hook|包/hook]`。
   - 开关是对齐 8 字节 slot 的原子写，已进入的调用正常返回。
   - DebugBus：`guest_patch status|select|enable|disable`。

## 包格式要点

与 shadPS4 相同的字段：`segments`（RX + 可选 RW）、`rebase64`、`exports`、`imports`、`hooks`、
`patches`、`bindings`、`counters`、`logs`，以及 `module_sha256`。

不同之处：

- hook 多了 `relocation`（`stolen`、`code`、`fixups`）。fixup 种类有三种：
  - `rel32`：RIP 相对，目标是模块偏移；
  - `abs64`：绝对跳转到模块偏移；
  - `abs64_local`：跳到跳板内部。
- site 的模式名为 `site-x86_64-sse`，只保存 FXSAVE 状态（x87、MXCSR、xmm0–15）。
  - 处理函数按 SSE2 编译，不碰 YMM 高半。
  - 因此 replace site 不能代替会清零 YMM 高半的 VEX 指令。
- 未实现（相对 shadPS4）：entry observer、import hook（PE 可改 IAT，留待需要时再做）、
  `module_signatures`、SDK 计数器写入 Foundation profiler（v1 只进状态文件）。

## 限制

- 只覆盖经内置 Steam 客户端启动的 x64 游戏。
  - 自重启游戏（MHW，`followSelfRestart`）走 Wine 镜像引导，只能注入一个 DLL，跳过补丁。
  - 32 位游戏不支持。
- 安装器在静态导入 DLL 的 DllMain 和 EXE 的 TLS 回调之后运行：这两类代码已经执行过，
  不能作为补丁目标。
- payload 没有 Windows 展开信息。异常不能穿过 hook 栈帧（例如 original 抛出、游戏在上层捕获）。
  作者要审核。
- payload 栈帧要远小于 4 KiB（没有栈探针）。
- 包没有签名沙箱，拥有游戏代码的全部权限，只运行自己审核过的包。

## 验证

- **PC 原生**（2026-10-15）：`tools/xrgame/guest-patch/test-probe.py` 全部 6 项通过。
  - 覆盖：带 call 和 RIP 相对指令的入口跳板、before/replace site、字节补丁、函数和数据绑定、
    计数器和日志，以及运行中关闭单个 hook。
  - 环境：Windows 11 x64，NDK 27.3 clang 18。
- **设备（Swan）**：Alyx 的两个探针包见 `tools/xrgame/guest-patch/games/546560/`：
  - `hlvr.exe` 入口；
  - 后加载的 `client.dll` 的 DllMain。

  结果见验证记录。
- **设备（Swan），第一个真实用途**（2026-10-16）：Alyx 的眼动注视点三个包，其中 `replace` site、`before` site 和带局部缓冲的 typed hook 在 `renderingpipelines.dll`、`rendersystemvulkan.dll` 上都装上并生效，见
  [记录](../validation/swan-alyx-etfr-guest-patch-20261016.md)。
