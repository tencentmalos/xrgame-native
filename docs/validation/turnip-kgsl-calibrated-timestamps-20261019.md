# Turnip KGSL 校准时间戳：用 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS` 实现 DEVICE 域（2026-10-19）

用户批准修改共享 Turnip（修法：用 ioctl 实现，并在初始化时探测；范围：游戏 Turnip 和 Steam 客户端 GPU 组件都用），也批准在 Swan 上验证。这个问题是排查官方 Steam 大屏 GPU 进程崩溃时发现的（见 [Swan 官方 Steam 记录](swan-official-steam-20261019.md)），但它不是那次崩溃的原因。

**结论：已修复，并在 Swan 上验证。**
- Steam 客户端组件（glibc Turnip + Zink）和游戏 Turnip（Android 版）在修复前都会给出错误的 DEVICE 时间戳，修复后都能读到正确值。
- 新库下，web helper 冷启动后的 `ZINK: vkGetCalibratedTimestampsEXT failed` 日志降为 0。
- `vkGetCalibratedTimestampsKHR` 和 `glGetInteger64v(GL_TIMESTAMP)` 都单调递增，速率与 `CLOCK_MONOTONIC_RAW` 一致。GPU 用 `vkCmdWriteTimestamp`/`glQueryCounter` 写出的时间戳，全部落在前后两次 CPU 侧读数之间。
- 测试期间没有 GPU 故障。
- 已按单一 Turnip 规则合入（用户同意）：
  - mesa `malos/main` 从 `adb7e30a` 快进到 `55186f1f`；
  - `build-turnip.sh` 和 `linux-steam/build-gpu.sh` 都 pin 到 `55186f1f`；
  - 游戏随 Turnip xrg17（`turnip-55186f1-xrg17`）更新；
  - 详见文末「合入」一节。

## 缺陷

mesa pin `adb7e30a`（`malos/main`，Turnip xrg13 起的基线）：

- `src/freedreno/vulkan/tu_knl_kgsl.cc` 的 `kgsl_device_get_gpu_timestamp` 只有 `UNREACHABLE("")`。release 构建里它不是断言而是未定义行为，返回值取决于编译结果。
- A840 的设备描述继承 `a7xx_gen3`，`has_persistent_counter = True`，所以 `tu_device.cc` 仍声明 `KHR_calibrated_timestamps`、`EXT_calibrated_timestamps`，以及（有 WSI 平台时）`EXT_present_timing`。
- `vk_common_GetCalibratedTimestampsKHR` 对 `VK_TIME_DOMAIN_DEVICE_KHR` 调设备的 `get_timestamp`，任一域失败就整体返回 `VK_ERROR_OUT_OF_HOST_MEMORY`。

两个构建里的实际表现不同（见下表），都不对：
- glibc 客户端组件里这个函数返回非 0，每次调用都失败。Zink 每取一次 GL 时间戳就记一行错误；当天下午测到 25.7 分钟 69,183 行，本次复测为冷启动后 8 分钟 5007 行。`GL_TIMESTAMP` 拿到的是一个不变的垃圾值。
- Android 游戏 Turnip（xrg16）里调用"成功"，但 DEVICE 时间戳恒为 0。

## 修复

补丁 `turnip-kgsl-calibrated-timestamps.patch`（SHA-256 `7945dc6f744e2ee088adfe96a8b0bede393f9bca4069ae27e3ba6148758bf422`，基于 `adb7e30a`，5 个文件）：

- KGSL：`kgsl_read_gpu_timestamp(fd, ts)` 发 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS`。参数是 1 个源 `KGSL_CALIBRATED_TIME_DOMAIN_DEVICE`（u32 数组）和 1 个 u64 结果。`kgsl_device_get_gpu_timestamp` 改为调用它；失败时返回 ioctl 的错误，不再走未定义行为。
- 新的物理设备字段 `has_gpu_timestamp`。KGSL 在 `tu_knl_kgsl_load` 里探测一次（读一次 DEVICE 时间戳），msm 和 virtio 后端设为 true（它们本来就用 `MSM_PARAM_TIMESTAMP` 实现）。
- 三个扩展改为在 `has_persistent_counter && has_gpu_timestamp` 时才声明。内核不支持时（老内核 ENOTTY、CX timer 未初始化 EOPNOTSUPP、权限被拒等），KGSL 就不再声明它们。

这个补丁就是 mesa 提交 `55186f1f0e739a507876bb3733cf00e1734c78f1`（父提交 `adb7e30a`；`git diff` 与补丁逐字节相同），已推到 `tencentmalos/mesa-mirror` `malos/main`。按 Mesa 的 AGENTS.md 规则，提交说明带 `Generated-by: LLM` 和 `biblioklept`，与 `adb7e30a` 相同。

### 内核依据

Qualcomm KGSL 源码 `qualcomm-linux/kgsl` `gfx-kernel.le.0.0` @`2b2d762c`（存档在证据目录 `kgsl-src-qualcomm-linux/`）：

- `adreno_ioctl.c` `adreno_ioctl_read_calibrated_ts`：
  - `sources` 是 u32 数组，`ts` 是 u64 数组，`count` 为 1–6；
  - `ADRENO_DEVICE_CX_TIMER_INITIALIZED` 未置位时返回 `-EOPNOTSUPP`；
  - 关中断后依次采样 MONOTONIC_RAW、DEVICE、MONOTONIC，`deviation` 是两次 MONOTONIC_RAW 之差。
- `adreno.c` `adreno_read_cx_timer`：DEVICE 域直接返回 CPU 的 `arch_timer_read_counter()`。
- `adreno_gen8.c` `gen8_cx_timer_init`：先测出写寄存器的开销，再把 CPU 计数器写进 `GEN8_GPU_CX_MISC_AO_COUNTER`，补偿这段开销，然后置位上面的标志。所以 GPU 的 always-on 计数器（Turnip 时间戳查询用的 `CP_ALWAYS_ON_COUNTER`，19.2 MHz）和 CPU 计数器是同一时钟。
- 系统 suspend/hibernate 时清掉这个标志，GPU 下次启动时重新校准。因此从 resume 到 GPU 第一次上电之间，ioctl 会返回 EOPNOTSUPP：
  - 恰好在这段时间里枚举设备的进程不会得到这三个扩展；
  - 已拿到扩展的进程在这段时间里调用，会像以前一样拿到错误码（不再是垃圾值）。

  Swan 上 XR runtime 一直在用 GPU，实际很难碰到。

## 构建

| 组件 | 方式 | 结果 |
|---|---|---|
| Steam 客户端 GPU 组件 | WSL 跑仓库配方 `tools/xrgame/linux-steam/build-gpu.sh`，补丁插在 `turnip-max-samples.patch` 之后（证据目录 `build-gpu-kgsl.sh`），构建根 `/root/kgsl-ts/recipe` | `libvulkan_freedreno.so` SHA `5ebb6b54…`，Build ID `4357482f125e66af3cb1c77a219789aa10e459e0`；`libGLX_steampsp.so.0`、`libgallium-26.3.0-devel.so` 的 Build ID 与现装组件相同（`0abb961d…`、`35bfa56e…`），只有 Vulkan 驱动变了。记录见 `client-gpu-build.json` |
| 游戏 Turnip | workbench-hub `docker exec xrgame-wp3` 跑 `tools/xrgame/build-turnip.sh`（用 pin 的 git archive，补丁同样插在第 5 个之后，输出改到 `/work/output/turnip-kgslts`，没动 xrg16 的输出；证据目录 `turnip-kgslts-remote.sh`） | Wine 用 `libvulkan_freedreno.so` `f08ccf6c…`（Build ID `caad0ffb…`），app 进程用 `libvulkan_freedreno_android.so` `71a315f1…`（Build ID `80a5defa…`）；构建脚本自带的 ICD/HAL 符号检查通过。上机验证用的是这一版；按新 pin 重新构建的 xrg17 成品见「合入」 |

## Swan 验证

设备：Swan `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，内核 `6.12.58-android16-6-maybe-dirty-4k`，boot id `69df66bf-d29e-4a0b-b384-89599ba4a88d`。测试程序都在 app 自己的数据目录里运行，跑完已删除。

### 1. 直接测驱动（`ts-test.c`、`gl-ts-test.c`）

`ts-test` 不经 Vulkan loader，直接加载给定的 ICD，分四步：
1. 看三个扩展是否声明；
2. 连续读 2 万次 (DEVICE, MONOTONIC_RAW, MONOTONIC)；
3. 隔 2 s 比较 DEVICE×`timestampPeriod` 与 MONOTONIC_RAW 的增量；
4. 50 次提交，每次用两次 calibrated 读数把 GPU 在管线头、尾写的时间戳夹在中间。

`gl-ts-test` 在 Steam 的 PRoot 里使用 web helper 同一套 GL 环境：glvnd vendor `steampsp`、`MESA_LOADER_DRIVER_OVERRIDE=zink`、`LIBGL_KOPPER_DRI2=true`。它建一个不带 drawable 的 GL 3.3 core context（不在头显上开窗口），连读 5000 次 `GL_TIMESTAMP`，测 1 s 速率，再用 20 次 `glQueryCounter` 做同样的夹逼检查。

客户端组件在 PRoot 里通过 `run-as` 运行（SELinux 域 `runas_app`）；游戏 Android 版直接通过 `run-as` 运行。

| 被测库 | 声明 KHR/EXT/present_timing | 2 万次读取 | 速率比 | GPU 时间戳在区间内 | 结果 |
|---|---|---|---|---|---|
| 客户端组件，现装 `28daac8c…`（修复前） | 1/1/1 | 20000 次全部返回 -1（`VK_ERROR_OUT_OF_HOST_MEMORY`） | — | 0/50（读回 0 和垃圾值） | FAIL |
| 客户端组件，新库 `5ebb6b54…` | 1/1/1 | 0 错误、0 回退，1.03 µs/次，deviation 平均 1.0 µs | 1.0000043 | 50/50 | PASS |
| 游戏 Android 版 xrg16 `1c7317f5…`（修复前） | 1/1/0 | 调用都"成功"，DEVICE 恒为 0 | 0 | 0/50 | FAIL |
| 游戏 Android 版新库 `71a315f1…` | 1/1/0 | 0 错误、0 回退，0.99 µs/次 | 1.0000037 | 50/50 | PASS |
| GL（Zink），现装库 | — | `GL_TIMESTAMP` 5000 次恒为同一垃圾值，Zink 打 5042 行错误 | 0 | 0/20 | FAIL |
| GL（Zink），新库 | — | 5000 次单调递增，1.03 µs/次，0 行 Zink 错误 | 1.000028（1 s） | 20/20 | PASS |

- GPU 写时间戳到 CPU 侧读数之间约 52 µs 到 10 ms，所以三个时钟（DEVICE 域、GPU 写出的时间戳、MONOTONIC_RAW）是同一时钟。
- 多次测量里，DEVICE×52.083332 ns 一致地比 `CLOCK_MONOTONIC_RAW` 快约 4 ppm（2 s 约 8 µs）。有一次因为单次调用被抢占，算出 30 ppm。这对时间戳用途没有影响，原因没有查。
- Android 版本来就没有 WSI 平台，所以不声明 present_timing。

完整输出见证据目录 `client-glibc-{before,after}.txt`、`game-android-{xrg16,kgslts}.txt`、`client-gl-{before,after}.txt`。

### 2. 官方 Steam 客户端（GPU 模式）

把客户端组件里的 `libvulkan_freedreno.so` 换成新库：用 `run-as` 以重命名方式替换，原文件留作 `.steampsp-orig`。

| 时段 | web helper GPU 进程用的库 | `vkGetCalibratedTimestampsEXT failed` 行数 |
|---|---|---|
| 18:24:03 冷启动 → 18:32:06（旧库） | 现装 `28daac8c…` | 5007（steamwebhelper.log 19,133 行中） |
| 18:35:15 只重启 GPU 进程 → 18:39:17（新库） | 新库（`/proc/<pid>/maps` 里 inode 262238） | 新增 0（累计 5934 不变） |
| 18:41:59 冷启动 → 18:44:06（新库） | 新库（inode 262238） | 0（8254 行中） |

18:41:54 的冷启动不是这次测试触发的：另一个会话在 Swan 上装了新 APK（1.2.1），系统 `Force stopping … due to installPackageLI`。装包没有覆盖 rootfs 里替换过的库。

**真实 app 进程确实走了新路径。** 用 tracefs 的独立 instance（`raw_syscalls:sys_enter`，只跟踪 GPU 进程的所有线程，5 s），在 `untrusted_app` 域的 GPU 进程里抓到 `ioctl(0xb, 0xc0200960, …)`，即 `IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS`，`count` = 1。Zink 只要调用失败就会记日志，日志里同期没有新增，说明调用成功。抓完 instance 已删除。

**SELinux。** 拉 `/sys/fs/selinux/policy` 用 `sesearch` 查：
- `allow untrusted_app gpu_device:chr_file { … ioctl … }`；
- 没有任何 `allowxperm` 规则限制 `gpu_device` 字符设备的 ioctl 号，所以 0x60 不会被拒。

`runas_app` 这一侧的结果已由上一节的测试覆盖。

### 3. GPU 故障

18:32 之后，`dmesg` 里没有 GPU page fault、hang、snapshot 或 context fault（匹配到的只有协处理器心跳 `reset-debug` 日志），logcat 里也没有 KGSL 相关的 SELinux 拒绝。审计日志有限速（`audit_lost`），所以 SELinux 的结论以上面的静态策略检查为准。

### 设备现状

- 验证后，`/opt/steampsp-gpu/lib/libvulkan_freedreno.so` 已还原为 APK 自带的 `28daac8c…`，不改变其他会话的测试环境。
- 当前那个 web helper GPU 进程（18:42 起）仍映射着已删除的新库，下次重启后就会加载原库，Zink 日志刷屏会回来，直到组件随配方重新构建。
- 测试文件（`files/kgslts/`）和 tracefs instance 都已删除。
- WSL 里装了 `setools`（只用于分析策略）。

## 合入（用户选定：按单一 Turnip 规则）

- **mesa。** 提交 `55186f1f` 以快进方式推到 `tencentmalos/mesa-mirror` `malos/main`（`adb7e30a..55186f1f`）。`feature/malos/xrgame-wine-icd` 和父仓库 gitlink（`4815dc71`）都没动，配方只用 pin。
- **配方。** `tools/xrgame/build-turnip.sh` 和 `tools/xrgame/linux-steam/build-gpu.sh` 的 `pin` 改为 `55186f1f`，补丁列表不变。`package-components.py` 的 mesa pin 和组件名同步更新。
- **游戏 Turnip xrg17。**
  - 在 workbench-hub 容器里跑新 pin 的 `build-turnip.sh`。只改了两个路径：参考仓库用 `apk-build/mesa-mirror`（经 SSH 拉到 `55186f1f`），输出目录用 `output/turnip-xrg17`。pin 检查照常执行。
  - 得到 `libvulkan_freedreno.so` `71a4d026…`（Build ID `2f0cbc5c…`）和 `libvulkan_freedreno_android.so` `fc7d70c0…`（Build ID `a4cf26e8…`）。
  - 它的源码树和上面上机验证过的那次构建（`adb7e30a` 加补丁）`diff -r` 一致，只有构建生成的 `__pycache__` 和一个指向 `AGENTS.md` 的悬空符号链接不同，见证据目录 `xrg17-srcdiff.txt`。二进制不同只是因为构建目录不同。
- **打包。**
  - `package-turnip-validation.py` 改为 xrg17：以 v14 APK 自带的 xrg16（`be6fbc76…`）为底，换上两个驱动，`sourcePins` 记为 `55186f1f`。
  - 得到 `turnip-55186f1-xrg17.zip`，SHA-256 `8d121b4668de07b638f91fcc8f5931e3a8cf9934ba6bf2d79162b19132dd9a4f`；去掉调试信息后的库为 `6bf2b004…` / `2d118ff0…`。
  - WSL 的 `/root/xrgame-inputs/bundle-turnip17` 是 v14 bundle 只换掉驱动的结果，由 `prepare-runtime-bundle.py` 生成，7 个组件全部校验通过，`manifest.json` `97a05f69…`。新 APK 用 `BUNDLE=/root/xrgame-inputs/bundle-turnip17` 出包。
  - 组件 URL `runtime-20261019-private/turnip-55186f1-xrg17.zip` 只是内部标识，没有发布。
- **app。** `XrGameRuntimeVersions.TURNIP` 改为 `turnip-55186f1-xrg17`。X11 AHB、`fdmviz`、`TU_MAX_SAMPLES` 三处驱动白名单都加上 xrg17，否则 ETFR 需要的 MSAA 上限和调试着色会被关掉。测试同步更新。
  - WSL 独立 worktree 里跑了 `:app:testPicoXrDebugUnitTest`：`XrGameMsaaTest` 2/2、`XrGameRuntimeTest` 6/6 通过。
  - 跑测试时排除了原生构建和 Linux Steam staging 两个任务，`build/xrgame-runtime/bundle` 指向 `bundle-turnip17`。`filterXrGameAssetsPicoXrDebug` 对默认组件的检查也通过了。
  - 没有出完整 APK。
- **客户端组件。** 新 pin 的 `build-gpu.sh`（含 `mesa-zink-killed-swapchain.patch`）在 WSL 构建通过，`libvulkan_freedreno.so` `fe392c6f…`（Build ID `099cbf68…`），GLX/gallium 的 Build ID 不变。记录见 `client-gpu-build-55186f1f.json`。

## 未完成

- **xrg17 成品还没上机。** 上机验证用的是同源码的补丁构建。Swan 同时在给另一个会话做测试，所以没有再用 xrg17 打包后的二进制和新 APK 重测。
- **没在设备上跑的部分。** Wine 用的游戏 Turnip（X11/AHB 版）没有单独上机测，它和 Android 版 KGSL 代码相同、同一次构建。游戏里实际调用的 vkd3d-proton `GetClockCalibration` / DXVK 也没测。长时间运行只覆盖了约 2–4 分钟。
- **另一处日志刷屏，与本修复无关。** web helper 每帧记一行 `eglGetMscRateANGLE: glXGetMscRateOML failed` 和 `extension "XFree86-VidModeExtension" missing on display ":1"`（旧库下 8 分钟 8307 行，新库下照旧）。
