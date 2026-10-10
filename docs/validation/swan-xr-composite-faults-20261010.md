# Swan XR 合成器 GPU page fault：分析、定位与修复（2026-10-09 至 10-10）

设备 Swan（Pico B3110，Android 16，Adreno 840），构建 `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0`。
游戏 Half-Life: Alyx，Vulkan 合成 + SGSR + 眼动 FDM。前情见[自动化与 HUD 记录](swan-xr-automation-hud-20261009.md#5-合成器-gpu-page-fault新线索未修) §5。

## 结论

**根因**
- A7XX/A8XX 有一组 bin foveation 寄存器：`GRAS_BIN_FOVEAT`、`RB_BIN_FOVEAT`，以及 A8XX 上每个视图的 `GRAS/RB_BIN_FOVEAT_XY` 和 `_XY_FDM` 偏移。它们控制渲染时按 bin 缩放和平移。
- Turnip 只在设备属性 `has_hw_bin_scaling` 为真时写这些寄存器。
- 2026-09-30 合入的 shadPS4 Swan 提交 `351a4847` 把 Adreno 840 的 `has_hw_bin_scaling` 改成了 False（上游默认为真）。从那以后，Turnip 在 Swan 上一次也不写它们。
- KGSL 普通上下文切换不保存、也不恢复上下文寄存器。别的 GPU 上下文留下的值（推测是使用专有驱动的 Pico 系统合成器或系统 UI），会原样作用在 Turnip 的下一个命令缓冲上。

**后果**
- 渲染目标的写入会额外写到图像之外：从第 2500 行开始，高为图高的 1/4，写入量是像素数的 1/16，写的是该渲染目标自己的像素。
- 落在地址空洞时，KGSL 报 page fault；落在其他分配上，就是静默的数据破坏。
- 合成器和游戏进程（Wine 里的 Turnip）都受影响。

**修复**
- 在 `tu6_init_static_regs` 末尾把这组寄存器清零。
- 提交 `tencentmalos/mesa-mirror` `malos/main` `adb7e30a`，合入 `feature/malos/xrgame-wine-icd` `4815dc71`。
- 运行时组件为 Turnip xrg13（`turnip-adb7e30-xrg13`）。同一份源码出两个库：游戏用的 X11/Android 库，和合成器用的 Android 库。

**验证**：结果见第 6 节。

## 1. 现象

- 合成器的 Turnip 上下文（app 进程，drawctxt 51）持续报 `GPU PAGE FAULT ... (write translation fault) FAULTING BLOCK: CCU`。
  - 地址多数落在 `(surface) was already freed` 的已释放区间里，或分配之间的空洞中。
  - 内核日志每 5 s 只放行 10 条，同时有上千条 `callbacks suppressed`。
- 只在超分重建开启时出现，`vr_upscale filter=off` 时为 0。
- 更早（10-09）还见过另外两类。当时没有联系起来，现在推断是同一机理：
  - 会话开始时，quad swapchain 末尾之后的 UBWC flag 被读越界；
  - 系统指示器弹出后偶发 CCU 写越界。

## 2. 分析：逐个排除

单靠内核日志，计数被限流，无法比较强弱。第 2 步起改用 tracefs 私有实例记录 `kgsl_mmu_pagefault` 事件，每一次 fault 都有记录（脚本见第 7 节）。

| # | 假设 | 方法 | 结果 | 结论 |
|---|---|---|---|---|
| 1 | 某个超分 pass 按输出尺寸写进了源尺寸图像（10-09 的猜测） | RD 抓帧，解码所有 RT、resolve 和 A2D blit 的基址、pitch、scissor | 全部在图像范围内，没有任何寄存器值超过图像高度 | 否 |
| 2 | FDM | 在线切换 fov=eye/off/fixed，每档 8 s 精确计数 | 写 501–728、读 411–565，三档相近 | 否 |
| 3 | 拷贝或绘制路径、HUD | 切换 `projection.blit`，开关 HUD | 都有 fault | 否 |
| 4 | GMEM 与 sysmem | `TU_DEBUG=sysmem`，RD 证实没有 bin | 仍然饱和 | 否 |
| 5 | 缓存回写或时序 | `TU_DEBUG=sysmem,flushall,syncdraw` | 40 s 内 80 条加 10146 条被限流 | 否 |
| 6 | fault 其实来自本进程的其他上下文（HWUI），被错记到 ctx 51 | tracefs 记录提交 | 3 s 内本进程只有 ctx 51 提交（301 次），HWUI 为 0 | 否，归属可信 |
| 7 | 命令缓冲内被抢占 | `TU_DEBUG=cmd_no_preempt` | 仍有：写 241–263、读 1035–1143 / 8 s | 否 |
| 8 | 让 GPU 在 fault 时停下抓快照 | `ft_pagefault_policy=0x3` | 快照缺少大部分 IB 内容，无法用 | 已恢复为 0x0 |

另外两个现象：
- FSR1 比 SGSR 多一个 RCAS pass，fault 也更多（写 1012、读 747）：写渲染目标越多，越界越多。
- 第 5 步之后有两次启动卡在「同步云存档」。原因是 Steam 云存档冲突对话框，与 GPU 无关；处理方式见第 7 节。

## 3. 定位

### 3.1 越界区域的形状

用 tracefs 记录本进程的 `kgsl_mem_alloc/map/free`，用 `vr_upscale out=` 让输出图重新分配，再把每个 fault 地址换算成它相对哪张图、按该图 pitch 的第几行。

| 输出 | pitch | 实测越界行 | 第 2500 行到 2500+H/4 |
|---|---|---|---|
| 1555x1440（out=60） | 6400 | 2504–2858 | 2500–2860 |
| 2332x2160（out=90） | 9472 | 2500–3031 | 2500–3040 |
| 2592x2400（out=100） | 10496 | 2500–3094 | 2500–3100 |

- 越界横跨整个 pitch。越界量（6.0 MB 对 3.4 MB）与图像面积成正比。
- swapchain（UBWC）的 flag 越界按 flag pitch 192 换算成像素行，也落在同一区间。
- prepared 图像（1296x1200）的越界在第 2553 行附近。
- 起点固定在第 2500 行，与图像高度无关；长度是 H/4。这不像任何一个 draw 写错了尺寸，更像硬件对所有渲染写入做了一次额外的「缩放加平移」。

### 3.2 最小复现（guard probe）

为了看到越界写的内容，在合成器里加了一个探针（`xr_vulkan_probe.cpp`）：
- 建一张图，内存后面多分配 4096 行 pitch 的保护区，填 0xCD；
- 第一个 pass 把纯色 (0x11,0x22,0x33) 画进去；
- 按变体决定第二步：采样它、拷贝它，或什么都不做；
- 提交并等待完成后，读回保护区。

原版 xrg12（run49、run50）：

| 变体 | 结果 |
|---|---|
| 0：非 MUTABLE（UBWC），第二步采样 | 4 轮中 2 轮，第 2511–2803 行，内容是压缩数据 |
| 1：MUTABLE 1296x1200，第二步采样 | 两次各 4 轮中分别有 2 轮和 3 轮，第 2503–2795 行，97200 个字（= 1296×1200/16），值全是 `ff7c6649`，即第一个 pass 的颜色经 sRGB 编码后的值 |
| 4：只渲染，不做第二步 | 4 轮中 1 轮 |
| 2：第二步改成拷贝 | 4 轮中 1 轮 |
| 8：第一张图 2592x2400 | 4 轮中 1 轮，第 2511–3107 行 |

由此得出三点：
- 越界写的内容是渲染目标自己的像素，数量是 1/16，即一份按 1/4×1/4 缩放的副本，被平移到第 2500 行。
- 只渲染一次也会出现，与之后怎么使用这张图无关。
- 时有时无，与此前 GPU 上运行过什么有关。

### 3.3 推到 bin foveation 寄存器

「1/4×1/4 缩放」「整体平移」「随其他负载时有时无」三个特征，正好对应 A8XX 的 bin foveation：

- `GRAS_BIN_FOVEAT`（A8XX 0x8206）有 BINSCALEEN，以及 6 个视图的 X/Y 缩放档位（1、1/2、1/4）。`RB_BIN_FOVEAT`（0x88f5）有 BINSCALEEN。
- `GRAS/RB_BIN_FOVEAT_XY[6]`（0x8220、0x8950）是 14 位的 X/Y 偏移，能装下 2500。`_XY_FDM[6]`（0x8228、0x8960）是 FDM 偏移。
- Turnip 写这些寄存器的地方都受 `has_hw_bin_scaling` 控制：
  - sysmem 和 GMEM 开始时的复位；
  - 每个 bin 的设置；
  - blit 路径。
- `351a4847` 把 A840 的这一项设为 False，所以这些写入在 Swan 上全部被跳过。`a8xx_base_raw_magic_regs` 只复位了 `GRAS_BIN_FOVEAT_XY_FDM`，其余都没管。
- KGSL 普通上下文切换不保存、不恢复上下文寄存器，每个上下文要自己写全它依赖的状态。Turnip 在 `tu6_init_static_regs` 里复位 `SP_PS_CNTL_0`，正是为了防止其他进程留下的值，属于同一类问题的已有处理。
- Pico 系统合成器（专有驱动）做注视点渲染，很可能用硬件 bin 缩放。不过「是哪个上下文留下的这些值」没有直接测量。

### 3.4 诊断验证

把复位加进 `tu6_init_static_regs`，在本地 WSL 构建合成器用的 Android 库（NDK r29，Build ID `3099a9b0…`）。

内置的 adrenotools 目录在每次启动前会被运行时校验还原（run50 验证了这一点：被替换的文件被改了回去，实际加载的是原版）。因此加了 `debug.xrgame.xr.turnipdir`，让合成器从单独目录加载。

run51 的结果：
- 探针变体 0、1、4、8 各 4 轮，共 32 个保护区全部完好；
- SGSR/FSR1、out=90/100、拷贝和绘制路径下，本进程 fault 都为 0；
- 全设备 20 s 和 60 s 都为 0。

## 4. 修复

`src/freedreno/vulkan/tu_cmd_buffer.cc`，`tu6_init_static_regs` 末尾：
- A7XX 及以上：`GRAS_BIN_FOVEAT`、`RB_BIN_FOVEAT` 清零。
- A8XX：另外把 6 个视图的 `GRAS/RB_BIN_FOVEAT_XY` 和 `_XY_FDM` 清零。
- A7XX：另外把 `GRAS_BIN_FOVEAT_OFFSET_0..3` 清零。

为什么放在这里：
- 这个函数在每个主命令缓冲开头（`tu_init_hw`）执行，也在 L1 抢占（skipsaverestore）切回后的 bin 恢复 preamble 中执行，两种残留都能覆盖。
- 开着 HW bin 缩放的设备会在每个 bin 重新设置这些寄存器，不受影响。
- 没有改 `has_hw_bin_scaling` 本身，也没有改 `351a4847` 的其他行为。

提交与配方：
- `tencentmalos/mesa-mirror`：
  - `malos/main` `04e1d665` → `adb7e30a`（`turnip: reset bin foveation registers in tu6_init_static_regs`）；
  - `feature/malos/xrgame-wine-icd` `57e87576` → `4815dc71`（合并上一条）。
  - 合并后的 `tu_cmd_buffer.cc` 与诊断构建的源码逐字节一致。
- 本仓：
  - `build-turnip.sh` 的 pin 改为 `adb7e30a`，3 个配方补丁不变，结果树等于 `4815dc71`；
  - `package-components.py` 的 pin 和版本名改为 `turnip-adb7e30-xrg13`；
  - `XrGameRuntimeVersions.TURNIP` 改为新版本，X11 AHB 名单和单元测试加入 xrg13；
  - gitlink 改为 `4815dc71`，并更新 `references/README.md`。
- 游戏进程：每次启动时 `XrGameRuntime.configureGraphics` 按哈希把当前版本的 `libvulkan_freedreno.so` 复制进 imagefs，再由 Wine 的 Vulkan ICD 加载。
- 合成器：从 `contents/adrenotools/<版本>/libvulkan_freedreno_android.so` 加载。
- 两者都跟随 `XrGameRuntimeVersions.TURNIP`。

## 5. 设备验证

**诊断构建**（run51）：合成器用本地 Android 库，游戏进程仍是 xrg12。

| 条件 | 本进程 fault |
|---|---|
| SGSR out=100，两次各 10 s | 0 |
| FSR1 / SGSR out=90 / out=100 | 0 |
| 绘制写 swapchain（当时的诊断开关），两次各 10 s | 0 |
| 全设备 20 s 和 60 s | 0 |

**xrg13**（run52）：
- **组件**：APK `a19510097f7b6a74c8ae4d47c144d460fa37f37a1b836fe5d2e1e3b285509a8c`，audit-apk 131 个文件、0 错误。
- **单元测试**：`XrGameRuntimeTest` 5/5、`XrDebugInputTest` 4/4、`SystemMetricsSourcesTest` 2/2、`WindowsVrGripCorrectionTest` 3/3。
- **两个进程都在用 xrg13**，用 `/proc/<pid>/maps` 加文件 SHA-256 核对，与包内 `xrgame-build.json` 一致：
  - 合成器（app 进程）：`contents/adrenotools/turnip-adb7e30-xrg13/libvulkan_freedreno_android.so`，`14ded1c6…`。
  - 游戏进程 `hlvr.exe`（以及 Wine 的 `explorer.exe`）：`imagefs/usr/lib/libvulkan_freedreno.so`，`fda9f3fb…`。
- **探针**：变体 0、1、4、8 各 4 轮，32 个保护区全部完好。
- **在线精确计数**：每档 10 s，本进程 fault 都为 0。覆盖的设置有：
  - SGSR out=100（两次）、FSR1、SGSR out=90、out=100；
  - fov=off、fov=eye；
  - 绘制写 swapchain（两次）。
- **全设备 300 s 计数**（12:57:30–13:02:30）为 0，包括游戏进程。
- **画面**：头显截图正常（`headset-xrg13.png`）。HUD 显示游戏 23–24 FPS，XR 72/72，GPU 99%。
- **状态**：Alyx 停在主菜单，没有长时间游玩。全 LRZ 的 GPU hang 还没在 xrg13 上复测。

## 6. `has_hw_bin_scaling = False` 的来历与现状

**现状**：`malos/main` `adb7e30a` 和 `4815dc71` 中，Adreno 840 仍是 False（`freedreno_devices.py` A840 条目）。xrg13 没有改它，只补了寄存器复位。上游 Mesa 中 A840 继承 `a7xx_gen3` 的 True，从未被设为 False。

**当时为什么改**：提交说明里没写原因，但 shadPS4 和 Azahar 的验证记录写了。在 Swan 上用 FDM 时，上游的硬件 bin 缩放路径渲染结果是错的：
- shadPS4：256×256、4×4 密度时只填了 64×64，像素矩阵大量失败；
- Azahar：4096×1024×2 时，每只眼只有左上 1024×256 有效。

改动的方式：
- shadPS4 的 `351a4847` 只把 A840 设为 False，走软件缩放（每个 bin 修补 viewport/scissor，缩放 bin 用慢速 store），保留 FDM；
- Azahar 的 `53515ac4` 在 `bin_scale_en` 上加了 `CHIP < A8XX` 条件，对所有 A8XX 关闭硬件缩放，并在 FDM pass 中关闭并发 binning。

两边的像素矩阵在这些改动后都通过了，记录里明确写着「硬件路径的原因未完全定位」。shadPS4 当时还试过复位 bin 寄存器，对那个 FDM 错误无效，就撤回了。那次试验和本次的跨上下文残留是两回事。

**副作用**：
- Turnip 写 `GRAS/RB_BIN_FOVEAT*` 的所有位置都受这个标志控制，包括 sysmem/GMEM 开始时的复位、每个 bin 的设置、不用 FDM 时的清零，以及 blit。设为 False 后，Turnip 在 A840 上一次也不写，由此产生本次的残留问题。
- 上游没有这个问题，因为 A840 为 True，每个 pass 开始时都会清零。

**是否正确**：
- FDM 渲染本身走的是上游在 a6xx/a730 上也用的软件路径，两组像素矩阵已在 Swan 上验证。
- 软件路径的代价是：缩放 bin 关 LRZ、用慢速 store、FDM pass 关并发 binning、每个 bin 由 CPU 修补。这些代价没有测量过。
- 加上 `adb7e30a` 之后，寄存器状态也可控了。`adb7e30a` 清的比上游更多：上游不复位 `_XY_OFFSET`，这里也清。
- 现在硬件缩放已经由 `CHIP < A8XX` 条件关掉，A840 的这个标志对原目的已是多余。
- **推断，未验证**：把 A840 恢复为 True、保留 CHIP 条件，可以和上游的每个 pass 复位保持一致，硬件缩放仍然关闭。改之前需要重新跑 FDM 像素矩阵和本次探针。
- 另外，CHIP 条件也关掉了 A830、A829 和 X2-85 的硬件缩放，这同样与上游不同。

来源：
- shadPS4 `docs/validation/android-native-host/xr-upscale-msaa-20260929.md`；
- Azahar `docs/manual/self/OpenXR/2026-09-28-swan-fdm2-validation.md`；
- 上游 `freedreno_devices.py`、`tu_cmd_buffer.cc`（2026-10-10 读取的 main）。

## 7. 方法与工具

- **精确计数**：在 tracefs 私有实例里只开 `kgsl/kgsl_mmu_pagefault`，事件带 `page=`、`pt=`（进程）和 `op=`，不受内核日志限流。脚本在 scratchpad 的 `rd/kgsl-pf.sh`。
- **地址对照**：同一实例里再开 `kgsl_mem_alloc/map/free`，再用 `vr_upscale out=` 让图像重新分配，就能拿到新分配的 VA 和大小。`debug.xrgame.xr.vktrace=1` 的 `xraddr` 日志把 VA 对应到 Vulkan 对象。
- **guard probe**：`debug.xrgame.xr.ccuprobe=<变体+1>`，日志 tag 为 `xrprobe`。变体位：1 是 MUTABLE，2 是第二步改为拷贝，4 是只渲染，8 是第一张图用输出尺寸。可以拿来做这个问题的回归测试。
- **本地 Turnip**：`debug.xrgame.xr.turnipdir=<目录>`。内置目录会在每次启动前被运行时校验还原，测试本地构建要用这个开关，只影响下次启动。
- **核对加载的驱动**：在 `/proc/<pid>/maps` 里找 `libvulkan_freedreno`，再对映射的文件做 `sha256sum`。游戏进程在 `ps` 里叫 `linker64`，参数里才是 `hlvr.exe`。
- **启动卡在「同步云存档」**：先看 logcat 里是否有 `SteamAutoCloud ... conflict resolution`。本次差异只是两个 VR 配置小文件。先把本地存档备份到仓库外（`save-backup-20261010-1112`），再选「保留远程存档」。
- **已删除的开关**：`debug.xrgame.xr.projection.blit` 在定位过程中区分了拷贝和绘制两条路径。用户在头显里看到绘制方式左右眼显示不全，用处不大，2026-10-10 已删除。

## 8. 遗留

- 全 LRZ 的 GPU hang 是否同源：LRZ 也在 GRAS 里，可能受残留的 bin 缩放影响。xrg13 现在已是全 LRZ（Alyx 容器只有 `TU_DEBUG=noconform`），需要长时间实际游玩来验证。
- 没有直接测量是哪个上下文留下的这些值（推测是 Pico 合成器或系统 UI）。
- 软件 FDM 路径的性能代价没有测量。
- 是否把 A840 的 `has_hw_bin_scaling` 恢复为 True，见第 6 节。

## 证据

| 轮次 | APK SHA-256 | 合成器 Turnip | 游戏 Turnip | app PID |
|---|---|---|---|---|
| run49 | `d6de4dafe1948f4714645c1fffcd13e20670989502751dcc1e640aacc269dd22` | xrg12 `e07c8618…` | xrg12 | 14588 |
| run50 | 同 run49 | xrg12（替换文件被启动校验还原） | xrg12 | 15264 |
| run51 | `65ed1fa7a5f8e1b14859ef58ee49d47dc22deafb2a9f2e97c55121f1b1d6a367`（`libxrimmersive.so` `1c8ab218…`） | 诊断库 `3099a9b0…` | xrg12 | 17884 |
| run52 | `a19510097f7b6a74c8ae4d47c144d460fa37f37a1b836fe5d2e1e3b285509a8c` | xrg13 `8882a3a7…` | xrg13 `1c924dc5…` | 24830 |

- run52 的组件：
  - Turnip 包 `turnip-adb7e30-xrg13.zip` `f039931c…`；
  - packages/bundle 的 catalog 和 manifest `49a55d78…`；
  - 其余组件与 xrg12 相同。
- xrg13 在远端 Linux 构建机的 `xrgame-runtime-build` 容器中按 `build-turnip.sh` 构建（NDK r27d）。配方源码中的 `tu_cmd_buffer.cc` 与推送的 `4815dc71` 一致（`af948f72…`）。
- 设备身份见文首；运行时长：run52 进入立体模式 12:52:08，验证持续到 13:07。
- 证据目录在仓库外，`xrgame-native-evidence/swan-20261008/swanxr-ahb/` 下的 run46b–run52：
  - tracefs trace、探针日志、logcat；
  - 补丁 `turnip-a8xx-bin-foveat-reset.patch`；
  - `which-turnip.txt`、`fault-counts.txt`、`pagefault-300s.txt`、截图；
  - catalog 和构建日志。
