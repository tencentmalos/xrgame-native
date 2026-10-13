# Swan：2D 大屏插帧（Foundation Lossless FG，2026-10-13）

在 Swan（Pico B3110）上，用 MHR（1446780）验证 2D 大屏模式的插帧：Foundation `modules/lossless_fg`
（重建的 Lossless Scaling 帧生成 shader 与调度图）接入大屏的 Vulkan 合成。

## 约束

- 插帧不能让游戏串行等待：X server 的渲染器和游戏都不等 XR 会话。
- 尽量不拷贝：真实帧直接作为插帧的输入和超分的源。
- 插帧在超分之前：真实帧和生成帧都在 Wine 屏幕尺寸上，经同一次重建（SGSR/FSR1）进入大屏图层。
- 参考 Azahar（`emulations/3ds/azahar` 的 `vk_lossless_fg*`）：只在游戏内容更新时推进插帧历史；生成帧排在当前真实帧之前；超分对两种帧只做一次。

上游 `VulkanRendererLsfg` 在 X server 的 present 路径上阻塞 acquire 交换链图像来插帧，大屏模式不用它。

## 改动

### X server 场景环

文件：`app/src/main/cpp/winlator/xr_frame_exchange.h`、`VulkanRendererContext`。

- XR 目标由 1 块 AHB 改为 5 块组成的环（`XrFrameExchange`）。
- 渲染器每帧选一块：会话没有占用，且不是最新帧的那块。
- 渲染器在 XR 模式下本来就会等自己的 fence。fence 返回后，它发布 `latest`（序号 + slot），同时记录完成时间和游戏内容计数。
  - 内容计数只在窗口内容更新时增加，包括 AHB、拷贝、采样和 CPU 上传。仅光标变化的帧沿用原计数。
- 没有可写的 slot 时，渲染器跳过这次合成，2 ms 后重试，从不覆盖会话正在读的 slot，也从不等会话。present 拷贝照常在同一循环里处理。
- 占用协议：
  - 会话先置占用位，再复查 `latest`。双方的这几次读写都用 `seq_cst`，所以要么会话看到更新的帧并放手，要么渲染器看到占用位。
  - 会话在自己的 GPU 提交完成后才释放占用位，按提交 serial 判断。
- 环随 Java 桥传递（`VulkanXrFrameBridge.onSceneExchange` → `XrNative.nativeSetSharedGameExchange`），双方各持一个引用。
- GLES 合成路径同样从环里取最新完成的 slot。

### 插帧

文件：`app/src/main/cpp/xrimmersive/xr_frame_generation.*`。

- 25 条 compute 管线，使用 Foundation 的内置 SPIR-V。设备支持 `shaderFloat16` 时用 FP16，否则用 FP32。合成设备按支持情况开启无格式 storage 写、扩展 storage 格式和 `shaderFloat16`。
- `frame.0/1` 直接绑定场景环的 slot（真实帧零拷贝）。其余 153 张中间图在 1280x720、光流 0.5 时共 15.0 MiB。
- 调度计划按 `frame_index % 6`、历史是否就绪、倍数和生成序号缓存。
- 同一列表内按读写冲突合并 barrier，列表开头放一次全局 barrier。
- 每个 submission 有自己的描述符池和 uniform 缓冲，submission 复用时重置。

### 大屏合成

文件：`xr_vulkan_compositor.cpp`。

- 每个 XR 帧只取最新的完成帧。插帧开启时，会忽略只有光标变化的帧（游戏 100 ms 内仍在出帧时才忽略）。
- 插帧开启时，新的游戏帧到达后：
  1. 按平滑后的游戏帧间隔和 XR 显示周期定倍数：`round(间隔 / 周期) - 1` 个生成帧，不超过所选倍数减一。
  2. 记录 shared pass。
  3. 生成帧与真实帧按「间隔 /（生成数 + 1）」均匀排开，生成帧在前。每个生成帧在要显示的那一 XR 帧才计算。
- 帧间隔超过 200 ms（暂停、加载）时重置插帧历史。
- 显示源（生成帧输出或真实帧 slot）统一交给 `reconstructQuad`，再 blit 到大屏图层。

### 设置

- 容器环境变量：
  - `XRGAME_XR_FRAMEGEN=off|2x|3x|4x`，默认 off。
  - `XRGAME_XR_FRAMEGEN_FLOW=25..100`，默认 50。
- 启动面板（仅 2D 大屏）新增「插帧」一行。选中后切到 Vulkan 合成，与超分的处理一致。「插帧光流分辨率」在高级选项里。
- DebugBus：`vr_upscale fg=… flow=…`，回复里带 `eye`、`fg`、`flow`。
- 状态层第一栏在插帧时显示 `GAME / FG Nx`，内容是游戏帧率 / 每秒显示的图像数。
- logcat 每 10 秒记一行 `frame generation Nx, game … fps, shown … fps, interval … ms`。

## 设备验证

| 项目 | 值 |
|---|---|
| APK | `002676838cf0271a7d9ac6b46c0fda3e5d710e2685b22d519d1740725a40560b`（run97） |
| `libxrimmersive.so` Build ID | `94b3627b83ba17bf5d3496fdd73d4fb87ff44aa5` |
| `libvulkan_renderer.so` Build ID | `408d921dfeb94a932d0f2415bcb85ff32adb0521` |
| 组件清单 | `49a55d7850a6b72757fdca1a3cb4fe6b53d30e5d4aced17ddea287e323f58145`（bundle-xrg13，Turnip xrg13） |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 进程 | app PID 22728；MHR 线程 26306/26413/26414（内核 audit 行） |
| 时长 | 13:58:25 启动到 14:01:38 退出 |
| 单测 | `WindowsVrUpscaleTest` 11/11 |

XR 刷新率为 72 Hz：请求 90 Hz 被运行时拒绝，系统当前只列 72。

插帧初始化：FP16 shader（fingerprint `a5cd7901…`），25 条管线，1280x720，光流 0.50，153 张图共 15.0 MiB。

### 同局 A/B（MHR 启动后的 30 fps loading 段）

每段 15 秒，用 DebugBus 切换：

| 时段 | 设置 | X server 游戏帧率 | 大屏 | 合成器统计 |
|---|---|---|---|---|
| 13:59:28–13:59:44 | `fg=3x` | 30.0 / 30.0 / 30.3 | 2x | 游戏 30.0–30.6，显示 59.7–60.2 fps |
| 13:59:50–14:00:05 | `fg=off` | 30.2 / 29.9 / 30.0 | 关 | — |
| 14:00:05–14:00:24 | `fg=3x` | 30.0 / 30.0 / 30.0 / 29.9 | 2x | 游戏 30.6，显示 60.2 fps |

- 开关插帧时游戏帧率不变，插帧没有让游戏串行。
- 72 Hz 下，33 ms 的游戏帧间隔约等于 2.4 个显示周期，取 2x。设定的 3x 是上限。
- 标题画面游戏约 60 fps，72 Hz 下取 1x，不生成帧，与设计一致。
- 前一包 run96（APK `45f41103…`，4 个 slot、无空闲时覆盖最旧）在游戏开始出帧的突发（X server 4188 fps）时打出过一次 `every slot held (held=0xd)`。run97 改为 5 个 slot 并跳过重试后，在 1629 fps 的突发下没有再出现。
- 两次运行都没有 KGSL fault，也没有 device lost。

截图 `fg-on-loading.jpg`（Pico 系统合成，插帧开启时的 loading 画面）显示正常，但 loading 画面近乎静止。

## 未验证

- 游戏场景内运动画面的插帧质量与伪影：MHR 约 27 fps 的场景在 72 Hz 下会取 3x。需要人工进入场景在头显里看。
- 场景内插帧的 GPU 开销，以及它对 GPU 受限游戏帧率的影响（这是 GPU 争用，不是串行）。
- 90 Hz 下的表现、2x/4x 档位，以及 `flow=25`（run96 在标题画面切过 `flow=25`，但那里不生成帧）。
- GLES 合成路径下的场景环（只做了代码路径，没有上机）。

## 证据

证据目录（不进仓库）：

- `swan-20261008/swanxr-ahb/run96-framegen/mhr-ab/`
- `swan-20261008/swanxr-ahb/run97-framegen-starve/mhr-loading-ab/`

内容包括 `marks.txt`、`logcat.txt` 和截图。

`lossless_fg` 的 shader 由 Lossless Scaling 的 `Lossless.dll` 逆向重建，见 Foundation 的 `analysis/PROVENANCE.md`。含插帧的 APK 只在内部分发。
