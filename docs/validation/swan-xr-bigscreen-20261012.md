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
  | **80 英寸（默认）** | 约 61° |
  | 100 英寸 | 约 73° |
  | 120 英寸 | 约 83° |

  - 原上游默认是 72 英寸 @ 2 m（1.6 × 0.9 m，约 44°），在 Swan 上偏小。用户先在 Swan 上看了 100 英寸，最后定默认为 80 英寸。
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

## GPU 直通

**根因**：上面那一轮日志停在 `direct-render bridge attached, waiting for its first AHardwareBuffer`，`directRender=false`，画面走 PixelCopy（CPU 读回）。Activity 已经把桥接交给了 `VulkanRenderer`，但 `setVulkanXrFrameBridge` 开头有 `if (!BuildConfig.XR_BUILD) return;`，picoXr 下渲染器根本没有建 XR 场景目标，也就不会产出共享 AHB。

**修复**：
- 去掉这个判断。桥接本来只在 `ImmersiveXrActivity` 显示游戏时才会设置，上游 Quest 构建行为不变。
- Windows VR 在立体开始前的平面阶段也会接上桥接；立体开始后，`applyFlatPresentationGate(false)` 停掉平面呈现。
- 状态层「GAME SCREEN」原先取 PixelCopy 的覆盖层尺寸（1920x1080）。直通时这只是覆盖层，现改为报告合成器实际显示的帧：直通时是共享缓冲，否则是捕获帧。

**设备验证**（同一设备、同一 boot id；MHR 都只到标题阶段，未进游戏场景）：

| 轮次 | APK | `libxrimmersive.so` Build ID | 进程 | 内容 |
|---|---|---|---|---|
| run70 | `281d8c703e0e5410ddb60861f0ce16da1f4f301fcba7f95f9e503a348fb7e7b7` | `820e0bea…` | app 25911，MHR 28986 | 去掉 `XR_BUILD` 判断 |
| run71 | `d94b3ae9d36e006e49764b9e5ec949c6dc5003b89593ce8e5dda9946481b4623` | `820e0bea…` | app 28717，MHR 30467；Alyx `hlvr.exe` 31016 | 默认 80 英寸；Alyx 回归 |
| run72 | `a7795c5fdb16d20ce13bc9edeae3ee014f0b7622dd116a6ec88d8f40c9ba71ee` | `79ca9a2d31e587312dae8670a4285a548adcb677` | app 28718，MHR 31955 | 状态层报告共享缓冲尺寸 |

- **直通生效**：三轮都有 `VulkanRenderer AHardwareBuffer observed (1280x720) — direct-render path active, PixelCopy of the game layer stopped` 和 `vulkan composite: shared game buffer imported 1280x720`，DebugBus `directRender=true`。MHR 容器的 X 屏幕是 1280x720，共享缓冲就是游戏的原生分辨率；之前 PixelCopy 读回的是 1920x1080 的视图。
- **XR 线程每帧 CPU 时间**（app 进程的 PxrMetric `FrmCpu` 中位数）：PixelCopy 时 0.80 ms（上一轮 app 23623，688 个采样），直通后 0.42 ms（run70，252 个采样）。两者是不同会话的标题阶段，GPU 负载没有做同局对比。
- **80 英寸**：XR 运行时报告大屏层 `QUAD 1280x720`、`1.771m x 0.996m`，即 16:9 的 80 英寸。
- **画面**：头显截图里 MHR 标题画面完整显示在大屏上，状态层在 run72 显示 `GAME SCREEN 1280x720`、GPU 30%。
- **Alyx 回归**（run71 包，无人值守脚本）：读档、扳机解除暂停后 `inScene` 稳定，立体 `Stereo active`，状态层游戏 32.3 fps、XR 90；场景内 PxrMetric 为 90/90、`LayerCnt=2`，无报错；`quickmenu exit` 正常退出。

## 刷新率

- run72 的会话停在 72 Hz（PxrMetric `72/72`，XR 运行时 `system display refresh rate 72`、`app request refresh rate 0`）。日志为 `Refresh rate 90 Hz not offered by the runtime`，运行时同时打印 `getDisplayRate 72.000000`。
- **原因**：Pico 的 `xrEnumerateDisplayRefreshRatesFB` 只返回显示屏当前的刷新率。我们原来只在 90 出现在列表里时才请求 90，所以只有系统恰好已经在 90 Hz 时才能拿到 90。这次 Alyx 退出后系统停在 72 Hz，之后的 MHR 会话就留在了 72。10-10 留存的沉浸式会话日志里，13 次都是 `90 Hz not offered`。
- **修改**：不再用列表做前提，总是请求设定的刷新率，日志同时记下运行时列出的值和请求结果。
- **验证（run74）**：系统在 72 Hz 时，运行时直接拒绝 90 Hz，日志为 `Refresh rate 90 Hz rejected by the runtime (-1, lists: 72)`，会话仍是 72 Hz。应用侧通过 OpenXR 改不了。
- **72 Hz 的来源**：Pico 系统的资源管理服务 `resmanagerserver`（日志标签 BrainCore）在 17:28:31 记录 `updateThrottleRefreshRate enable=1 rate=72`，并把 `sdk_refreshRate` 设为 72 通知全系统。当时 Alyx 在场景里，PxrMetric 的 GPU 占用为 99%。这个节流之后一直没解除，后续所有会话都是 72 Hz。触发条件（温度还是负载）在日志里看不到。
- 修改仍保留：系统回到 90 Hz 后照常请求 90，而且日志能看出运行时当时给了哪些值。

## 状态层位置（run74）

用户反馈状态层带着朝向人眼的偏转，与主屏幕放在一起显得别扭，要求改成平行平面，不遮挡画面。

- **大屏模式**：
  - 状态层与主屏幕同朝向，左对齐，放在屏幕上沿外侧，间隔为状态层高度的 1/4。
  - 每帧跟随屏幕的位置和大小，运行中移动或缩放屏幕会一起走。
  - 宽度按 Alyx 下的视角大小（2.5 m 处 1.6 m）随屏幕距离缩放，且不超过屏幕宽度。
- **Alyx（立体）**：位置不变（头部偏航坐标系里，前方 2.5 m、左 0.9 m、上 1.0 m），朝向只保留头部偏航，保持竖直，平行于正前方的平面，不再朝眼睛偏转或俯仰。
- **GPU 占用一栏**：Foundation 优先读 KGSL 的 `gpubusy`，Swan 在硬件调度下这两个计数一直是 `0 0`，同一时刻 `gpu_busy_percentage` 为 78%。我们的文件源跳过 `gpubusy`，让它改用 `gpu_busy_percentage`，Foundation 本身没改。

| 项目 | 值 |
|---|---|
| APK | `44ad85d73a4a30c56a0daf351c4d8aaa17c677a3857259a8fd4f07204ed4828d` |
| `libxrimmersive.so` Build ID | `b77eaa79a0d40c96e1939e4230374365d167f56b` |
| 进程 | app 31671，MHR 9108，Alyx `hlvr.exe` 9730 |

- **MHR**：
  - XR 运行时列出两层，Layer0 为 `1.771m x 0.996m`（80 英寸屏幕），Layer1 为 `0.960m x 0.120m`（状态层）。
  - `vr_hud status` 的状态层中心为 (-0.41, 0.59, -1.5)，与屏幕左上角外侧的计算值一致。
  - 头显平放在桌上，截图视角偏下，没有拍到屏幕上方的状态层。
- **Alyx**：
  - 无人值守读档进场景，立体正常，状态层游戏 31.6 fps，放置在 (-1.52, 0.78, -2.98)。
  - 菜单退出正常。
  - 是否看着自然，还要戴上头显确认。

## MHR 帧率定位（run72 包，标题后阶段）

用户观察：状态层上 CPU/GPU 都不高，但帧率只有 27 左右。

测量条件：
- 锁频用 spatial-debug-tool 的 `swan-lock-current-max`，即系统当前上限：CPU 0–5 为 2.11 GHz，CPU 6–7 为 1.50 GHz，GPU 902 MHz；硬件上限分别为 3.63 GHz、4.61 GHz、1200 MHz。
- 会话为 72 Hz，MHR 渲染 1280x720。

- **帧**：游戏 27.3 fps，帧周期中位数 36.6 ms。
- **GPU**（KGSL trace 8 s）：
  - 整机 GPU 忙 77%，其中 MHR 自己的 VKD3D 队列（ctx 43）占 73.7%；我们的合成器和 Pico 合成器合计约 3%。
  - MHR 每帧在 GPU 上连续忙 27.4 ms（中位数），帧与帧之间 GPU 空闲约 9 ms。
- **CPU**：游戏主线程单核 89%（CPU2，2.11 GHz），提交线程 46%，8 个任务线程各 11–17%。状态层的 CPU 一栏是 8 核平均，一个核跑满只算 12.5%。
- **判断**：
  - 游戏没有让下一帧的 CPU 工作与这一帧的 GPU 工作重叠，每帧约为 GPU 27 ms 加上 CPU 串行的约 9 ms。
  - 即使完全重叠，902 MHz 下 GPU 只够约 36 fps；主线程每帧约 32 ms，也只够 30 fps 左右，两边接近同时受限。
  - 两边都低于硬件上限，是系统策略压的频率，所以锁到系统上限不会提高帧率。
  - 主线程的 89% 里有多少是自旋等待，还没有采样确认。
- 状态层原来显示的 GPU 30% 或 `--` 来自上面说的 `gpubusy` 问题，不是真实负载。

## VST 透视（run76）

只做 2D 大屏，VR 游戏不处理。实现参考 Azahar 在 Pico 和 Swan 上的做法。

- **原生层**：
  - 运行时名称含 "pico"，或支持 `XR_BD_controller_interaction` 时，视为 Pico 运行时。Swan 报告 `Pico XRRuntime()`。
  - Pico 上照常启动 `XR_FB_passthrough`，但不提交透视层，混合模式保持 `OPAQUE`，由运行时在应用的所有层后面画透视。其他运行时仍提交透视层并使用 `ALPHA_BLEND`。
- **设置**：「2D 大屏（XR）」下新增「VST 透视」开关，默认关。它存为容器变量 `immersivePassthroughEnabled`，与游戏中控制面板的透视开关是同一个状态。
- **Manifest**：声明 `com.oculus.feature.PASSTHROUGH`（非必需）。没有加相机权限，Azahar 的相机权限是给 3DS 摄像头模拟用的。

| 项目 | 值 |
|---|---|
| APK | `c0b4977b32b0420049589da983b5579a9bb7bf3665c837135a899a7262fdc7d7` |
| `libxrimmersive.so` Build ID | `949fd211f1a006752584f8dd2446d4a28b46c0e6` |
| 进程 | app 14129 |

- **启用**：Hades II 容器原来已是大屏模式（80 英寸、1.5 m、90 Hz）。这次在容器配置里加上 `immersivePassthroughEnabled=true`，原配置备份在仓库外。
- **日志**：
  - `OpenXR runtime "Pico XRRuntime()"; passthrough drawn by the runtime behind the layers`
  - 运行时 `xsi.is_mrPassthroughEnabled = 1`
  - `Passthrough started (runtime background)`
- **画面**：头显截图里，大屏以外是摄像头拍到的真实环境，办公桌和显示器清晰可见；状态层在屏幕左上角外侧。
- **与 VST 无关的报错**：Pico 主界面进程反复报 `xrSetEnvironmentPassthroughPreferencePICO ... XR_ERROR_VALIDATION_FAILURE`。没开透视的 MHR 会话里同样存在（run74，约 55 分钟共 5.2 万条），只要沉浸式会话在跑就会出现，原因未查。

## 外接手柄（run77、run78）

用户反馈：大屏模式下，外接的 Xbox 手柄与 XR 手柄冲突。

**原因**：
- **事件没转发**：沉浸式界面没有把实体手柄的事件转给游戏输入，本局里 Xbox 手柄的按键一次都没到达 WinHandler（`Key routed` 为 0）。
- **槽位被占**：`ControllerManager` 把 1 号玩家分给了 Pico 用 XR 手柄生成的虚拟手柄 `pvr-virtual-input-0`，Xbox 手柄排到 2 号。
- **状态被覆盖**：`XrGamepadBridge` 每次轮询（约 90 次/秒）都把 XR 手柄的状态写进 1 号玩家，空闲时也会覆盖别的输入。

**改法：哪边按下就切到哪边**：
- **实体手柄的事件**：沉浸式界面接管 `dispatchKeyEvent` 和 `dispatchGenericMotionEvent`。Pico 的 `pvr-virtual-input`/`pico-virtual-input` 不算，它们与 OpenXR 送来的 XR 手柄是同一份输入。
  - 菜单或暂停界面打开时，按主界面的方式交给菜单导航。
  - 游戏中，只更新这个手柄自己的状态。
- **切换规则**：
  - 实体手柄按键，或摇杆、扳机、方向键超过 0.35，就成为 1 号玩家的输入。
  - XR 手柄有按键，或摇杆、扳机超过 0.35，就切回 XR 手柄。
  - 生效的一方空闲时，另一方不会覆盖它。XR 指针模式下，如果当前是实体手柄生效，它照样驱动游戏。
- **快捷菜单**：
  - 实体手柄的 Start 键与 XR 手柄的 Menu 键规则相同：按住 600 ms 开关快捷菜单；短按给游戏发一次 Start。
  - Xbox 徽标键（`KEYCODE_BUTTON_MODE`）收到时，直接开关快捷菜单。
- **震动**：实体手柄生效时，1 号玩家的震动改发到这个手柄（`WinHandler.setRumbleDeviceOverride`）；切回 XR 手柄后恢复原来的去向。
- **状态层**：改成 5 列，新增「INPUT」一栏，显示当前生效的输入（`XR controllers`、`Xbox pad`、`DualSense`），`vr_hud status` 中同样带 `input` 字段。

| 项目 | run77（切换） | run78（菜单、震动） |
|---|---|---|
| APK | `49434d4d32ad4b4012ade1a3da21a616bb2736851d32562c58dc21b2c6724c73` | `3559e10a4f468ae119b7e77cd6f77d72dc8efd1211ee795e44c1e773ac1068ff` |
| `libxrimmersive.so` Build ID | `a13ecc1647f647cc2c060085af56d797243836b9` | 同左 |
| 进程 | app 16114 | app 16723，被系统界面强制停止后，用户重新打开为 18914 |

- **run77**：19:20:08 记录 `game input switched to Xbox pad`，状态层 INPUT 显示 `Xbox pad`，Hades II 59.9 fps。
- **run78**：
  - 日志记录了 19:27:51 切到 DualSense、19:28:30 切到 Xbox pad。同时连着的 PS5 手柄当时显示为截断的 `DualSense Wirele`，之后已改为 `DualSense`，这个改动还没上机。
  - adb logcat 在 19:31:55 断开，之后的测试没有日志。
  - 用户在头显里确认：Xbox 和 DualSense 都能正常游戏，长按 Start 能开关快捷菜单，震动也正确。这部分没有日志佐证。
- **没做**：XR 手柄自身的震动（OpenXR 触觉反馈）。

## 尚未解决或覆盖

- 72 Hz 节流由 Pico 系统的 BrainCore 决定，触发和解除条件未查明。在它解除之前，所有会话都是 72 Hz。
- 未做硬件上限频率下的对比（CPU 3.63/4.61 GHz、GPU 1200 MHz），所以 CPU 和 GPU 各自对帧率的影响还没有分开测。
- DebugBus `quickmenu exit` 偶尔不生效：确认键没被退出项接收（`dispatchKeyEvent=false`），发生在用户刚手动操作过菜单之后；下一次就正常了。
- 只看了 MHR 的启动画面和标题阶段，没有进游戏场景测帧率和功耗，也没有做长时间游玩。
- 快捷菜单里的大屏操作和手柄指针没有系统测试。run72 中，有人在头显里试过用指针抓屏幕边缘，日志记为 `not in a grab zone`。
- run72 截图那一帧状态层的游戏 FPS 显示为 213.6，此后 DebugBus 连续采样都在 30 左右，来源未查。
