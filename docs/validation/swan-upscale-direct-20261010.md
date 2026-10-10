# Swan：SGSR 直接重建到 swapchain 层（2026-10-10）

设备 Swan（Pico B3110，Android 16），游戏 Half-Life: Alyx（546560），OpenXR 模式，Vulkan 合成，SGSR + 眼动 FDM，输出 100%（2592x2400），Turnip xrg13，OpenComposite `8a8b65ea`（只拷显示区域）。

## 改动

- **原流程**：每只眼在 SGSR 之外还有两次全尺寸处理：
  - 准备 pass：从传输 AHB（3670x1699）里把这只眼的 1296x1200 区域拷成一张中间图；
  - `vkCmdCopyImage`：把 2592x2400 的重建结果拷进 OpenXR swapchain 层。
- **Foundation**：`4a2fbf4`（分支 `feature/malos/xrgame-upscale-direct`，从固定提交 `bcbbd5e` 切出）新增 `VulkanUpscaler::RenderTo`。
  - SGSR 直接从 AHB 读这只眼的区域，直接写调用方给的单层 view。
  - 区域内部用 `textureGather`。区域边界改用 clamp 后的 `texelFetch`，等效于原来对中间图做 clamp-to-edge 采样，另一只眼和传输边框的像素不会被采到。
  - 目标是 sRGB 时，着色器先输出线性值，再由硬件编码。
  - 只有满足以下全部条件才走新路径：SGSR、输入已是 sRGB 编码、区域对齐到像素、缩放为正（没有翻转）。否则什么都不录制、返回 false，调用方仍用 `Render` 加拷贝，FSR1 也走这条老路径。
  - 原有四个 SPIR-V 用 glslang 15.1.0 重新生成，结果逐位一致。
- **app 侧**：`xr_vulkan_projection.cpp` 优先调用 `RenderTo`，渲染目标是 swapchain 第 `eye` 层的 view。销毁 swapchain view 之前先 `ReleaseTargets`。投影日志新增 `direct=` 计数。
- **诊断开关**：
  - `debug.xrgame.xr.upscale.copy=1`：恢复「`Render` 加拷贝」的老路径，用于同一局里对比；
  - `debug.xrgame.xr.upscaleprobe=1`：在设备上运行一次等价性探针。

| 项目 | 值 |
|---|---|
| APK | `6a048f3c4e10752dad0ba484189b893548ec8730b7cbd8011949bf51a4e4c673` |
| `libxrimmersive.so` Build ID | `f37a2e38dab62bebfefa36c52db4c30e3183ab8c` |
| 组件清单 | `49a55d7850a6b72757fdca1a3cb4fe6b53d30e5d4aced17ddea287e323f58145`（bundle-xrg13） |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 进程 | app PID 29744，`hlvr.exe` PID 2553 |

## 等价性探针（18:27，主菜单阶段）

- **输入**：合成的双眼图集，3670x1699。左眼区域放竖条纹和噪声，右眼区域放另一种图案，两只眼以外的区域涂品红。
- **对比的两条路径**：`Render` 加拷贝（之前的做法），与 `RenderTo` 写入 `R8G8B8A8_SRGB`（格式 43）、2592x2400 的图像。
- **方法**：两边都读回，逐字节比较。

| 眼 | FDM | 不同的字节 | 差值超过 1 的字节 | 最大差值 | 输出中的品红像素 |
|---|---|---|---|---|---|
| 左 | 关 | 791441 / 24883200（3.2%） | 0 | 1 | 0 |
| 左 | 开 | 3308209（13.3%） | 246736（1.0%） | 3 | 0 |
| 右 | 关 | 1227442（4.9%） | 154 | 2 | 0 |
| 右 | 开 | 2542194（10.2%） | 230892（0.9%） | 4 | 0 |

- 没有 FDM 时，差异是 sRGB 解码后再由硬件编码带来的舍入误差。
- 开 FDM 时差值较大，推测是低密度片元写回全分辨率图像时，驱动在 sRGB 和 UNORM 两种目标上的处理方式不同。这一点没有单独验证。
- 两条路径都没有读到眼睛区域以外的纹素。

## 场景内 A/B（18:32–18:36，同一局）

- 场景：读档 `a1_intro_world` 并恢复游戏后，头显静置。
- 方法：交替切换 `debug.xrgame.xr.upscale.copy`，0 为直接写入（B），1 为老路径（A）。共 3 轮，每种状态采 2 段，每段 8 秒，切换后等 5 秒。
- 测量手段和其他记录相同：KGSL tracefs 统计各 GPU 上下文的独占时间，游戏帧率按桥接提交次数计算，GPU 上限 902 MHz。

| 状态 | 游戏帧率 | 每个新帧的合成耗时 | 合成占 GPU | 游戏每帧 GPU 时间 |
|---|---|---|---|---|
| 直接写入（B，6 段） | 平均 29.95 fps（29.4–30.4） | 1.30 ms | 3.9–4.1% | 28.82 ms |
| `Render` 加拷贝（A，6 段） | 平均 27.82 fps（27.4–28.2） | 3.21–3.23 ms | 9.1–9.5% | 29.14 ms |

- 每个新帧的合成耗时减少约 1.9 ms（−60%），游戏帧率提高约 7.7%。每一轮里 B 都高于相邻的 A。
- 两种状态下游戏每帧 GPU 时间基本相同，说明场景状态一致；省下的 GPU 时间都给了游戏。
- 这一局的视角负载比 run60 重：游戏每帧约 29 ms，run60 约 20 ms，所以绝对帧率比 run60 低，不能跨局直接比较。
- 测量期间 logcat 里没有 KGSL page fault。
- 测完已恢复 `upscale.copy=0`，`upscaleprobe=0`。

## 尚未覆盖

- 没有做长时间游玩，也没有测 Alyx 动态分辨率变化时的情况；只在 Swan / Turnip xrg13 上验证过。
- FSR1 和区域翻转（如 OpenGL 游戏）仍走老路径。
- 桥接的 AHB 拷贝还在，每帧约 0.37 ms，在游戏上下文里执行。
