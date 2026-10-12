# Swan：普通游戏的 2D 大屏（XR）模式（2026-10-12）

在 Swan（Pico B3110）上，用 MHR（1446780）验证普通 Windows 游戏通过 OpenXR 会话以大屏显示。

## 背景

- picoXr 之前在头显上启动普通游戏，走的是 MainActivity，由 Pico 系统作为 2D 应用窗口显示，没有进入 OpenXR 会话。
- 沉浸式 Activity 本来就支持普通游戏的 quad 大屏，画面经 VulkanRenderer 共享 AHB 或 PixelCopy 送入。但这条路径只在上游的 `XR_BUILD` 风味里启用。

## 改动

- **启动**：头显上，容器的 `launchImmersiveMode` 为真时进入 `ImmersiveXrActivity`，不再依赖 `XR_BUILD`。
- **设置**：「显示模式」新增「2D 大屏（XR）」，只在头显上提供，与 VR 模式、SBS 影院互斥。选中后显示 XR 刷新率和「大屏尺寸」两项。上游 XR 构建的原有开关行为不变。
- **大屏尺寸预设**：16:9，全部放在 Swan 的 1.5 m 虚拟焦距上，选项里标出水平视角。

  | 尺寸 | 水平视角 |
  |---|---|
  | 65 英寸 | 约 51° |
  | 80 英寸 | 约 61° |
  | **100 英寸（默认）** | 约 73° |
  | 120 英寸 | 约 83° |

  - 原上游默认是 72 英寸 @ 2 m（1.6 × 0.9 m，约 44°），在 Swan 上偏小，用户确认要更大。
  - 运行中手动缩放或移动后，设置里显示「自定义」。
  - 预设只在大屏模式下写入容器，SBS 影院的尺寸和距离不受影响。
- **刷新率**：XRGAME 新建容器默认 90 Hz（用户确认 Swan 应为 90 Hz）；上游 XR 构建仍为 72 Hz。已有容器保留各自存的值。
- **状态层**：与 Alyx 相同的世界锁定性能 HUD。
  - 大屏模式默认使用 Vulkan 合成；容器里设置的 `XRGAME_XR_COMPOSITE` 优先。
  - 游戏 FPS 取 X server 实际呈现的帧率，即 2D 模式 HUD 的同一来源，由 Activity 每 500 ms 传入。
  - 最后一栏在大屏模式下显示为「GAME SCREEN」，内容是画面尺寸。

## 设备验证

| 项目 | 值 |
|---|---|
| APK | `805644c3f31ae9fc7474068751072b00df7162750746782a40c43321d93df12c` |
| `libxrimmersive.so` Build ID | `820e0bea2f80a97f81b57b69f6990d003a17359a` |
| 组件清单 | `49a55d7850a6b72757fdca1a3cb4fe6b53d30e5d4aced17ddea287e323f58145`（bundle-xrg13） |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0` |
| 进程 | app PID 23623，`MonsterHunterRise.exe` PID 26266 |

- **准备**：MHR 容器的显示模式设为 2D 大屏（XR）、90 Hz，与在设置里选择等效，原配置已备份到仓库外。用 DebugBus `launch 1446780` 启动。
- **会话**：`OpenXR immersive session initialized (1280x720 quad)`，请求 90 Hz，PxrMetric `FPS=90/90`，进入时游戏库主面板自动退到后台。
- **大屏尺寸**：默认 100 英寸 @ 1.5 m 生效。
- **状态层**：Vulkan 合成，`vr_hud status` 为 `gameFps=30, xrFps=90`。头显截图里，状态层显示游戏 30.0 fps、XR 90.0 / 90，以及 CPU、内存、电池、温度和 `GAME SCREEN 1920x1080`。截图那一帧 GPU 占用一栏为 `--`。
- **单元测试**：`XrScreenPresetsTest` 4/4，打包检查 131 项、0 错误。

## 尚未解决或覆盖

- **GPU 直通没有生效**：日志停在 `VulkanRenderer detected — direct-render bridge attached, waiting for its first AHardwareBuffer`，`directRender=false`，画面走的是 PixelCopy（CPU 读回）。进入实际场景后的帧率和 CPU 开销未测。
- 只看了 MHR 的启动画面和标题阶段，没有做长时间游玩，也没有试快捷菜单里的大屏操作和手柄指针。
- 状态层的 GPU 占用为 `--` 的原因未查。
