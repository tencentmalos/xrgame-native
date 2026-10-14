# Swan：沉浸式会话截图与录屏（scrcpy capture SDK，2026-10-14）

起因：头显上的系统截图取不到 VR 会话的内容（`capture_headset_view` 的 Pico 系统合成有时不含应用的图层，面板镜像带镜头畸变）。用户要求接入 scrcpy capture SDK、完善录屏，并参考 GFXReconstruct 处理 OpenXR 图层的录制。用法与实现见 [DebugBus 说明](../debugging/debugbus.md) 的「截图与录屏」一节。

## GFXReconstruct 的参考结论

固定的 `references/gfxreconstruct`（`3868cd1`）能作为 OpenXR API 层录制：它拦截 swapchain 和 `xrEndFrame`，逐字段记下各层结构，并把各层空间定位到 VIEW 空间，回放时再还原。它录的是 API 调用而不是像素，而且 OpenXR 部分要求它的 Vulkan 层也在调用链里；SteamPSP 的合成 Vulkan 是 adrenotools 直接加载的 Turnip，不经 Vulkan loader，所以不能直接拿来录屏。借用的做法：

- 在图层自己的命令缓冲里、`xrReleaseSwapchainImage` 之前拷贝（GFXR 回放端的 proxy 拷贝）。
- 跟踪每层最后释放的图像：投影层在截取帧上不复用旧图，保证拷到的是正在显示的那张。
- 按 `xrEndFrame` 实际提交的层结构合成，各层空间用 `xrLocateSpace` 统一到 LOCAL，眼睛视图在 displayTime 定位。

合成着色器改编自 shadPS4 的 `xr_capture`（GPL-2.0-or-later）。通用的 OpenXR API 层（录不属于我们的应用）没有做：Android 上要改 APK assets 或 root 写 `/vendor`，拿不到应用的队列锁和渲染时机。

## 环境

| 项目 | 值 |
|---|---|
| 设备 | Swan（Pico B3110），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys` |
| boot id | `6d4335a5-667c-426d-815b-1a82edaa0fbb`（两轮运行在同一次开机内） |
| 运行时 bundle manifest | `8b3e4fc78fc05816c2e089f8c0c27c2b5175b4782cfe84fbef8c5b8c5e548470`（DXVK xrg6、VKD3D xrg3、Turnip xrg13） |
| 眼视图 | 运行时推荐 2592x2400；截取单眼 1296x1200 |
| Foundation | 固定提交 `4a2fbf4` 加本地分支 `feature/malos/xrgame-imgui-capture` 的未提交改动（`XrImguiVulkanLayer` 截取目标） |
| capture SDK | `tencentmalos/my_mcp_tools` `8a0052b`（`dev_tools/mcp/scrcpy/capture-sdk/native`，未改动） |
| 工具 | `capturectl.py --app steampsp`（my_mcp_tools 工作区内未提交的改动，加了 `--app/--package/--service`） |

| 运行 | APK | `libxrimmersive.so` Build ID | 内容 |
|---|---|---|---|
| run107（Alyx） | `cd625a1f9232eb5236a786090279157ce37d534330cfb34c2fbed63b86e28afc` | `a87b2a72a2c861ed8b2e39a78957c5bd7fb1c716` | 首版；状态层只在有截取请求后才挂目标图 |
| run108（Hades II） | `4187a427016383401eae60c54a25cb109bf0f0fbb0df43a2ce09f228f3b27ae3` | `9eb151df00ce4023b227804bf3f13d7d1ef57302` | 状态层与键盘整个会话都挂着目标图 |

两个 APK 都通过 `audit-apk`（131 个文件，0 错误）；`libxrgame_debugbus.so` 均为 `b30822c29afce885204dc29fae36e99b10bfa1bc`。证据（PNG、H.264、MP4、`.frames.csv`、logcat）在仓库外 `xrgame-native-evidence/swan-20261008/swanxr-ahb/run107-capture` 和 `run108-capture-bigscreen`。

## 结果

会话外（app 进程，无沉浸式会话）：`capture_source` 返回 `status: ok`；`capture_screenshot request` 返回 `failed / no immersive Vulkan composite session`；`capture_video status` 为 `idle`。会话开始时日志 `capture: session generation 1, encoder surfaces available`，即合成实例与设备经 `xrCreateVulkan*KHR` 接受了 `VK_KHR_android_surface`/`VK_KHR_swapchain`。

### run107：Half-Life: Alyx（Windows VR 投影），PID 16783

无人值守进场景（`launch 546560 load=s0/autosave`，10:11:43 `inScene` 稳定），随后：

| 步骤 | 结果 |
|---|---|
| `xr` 截图，双眼 | 2592x1200 `stereo_sbs` PNG；电梯按钮面板在两眼中有正确的视差，无畸变 |
| `xr` 截图，左眼 | 1296x1200 PNG |
| `xr` 录像 8 s | 提交 286 帧、跳过 10 帧（编码器忙）、编码 287 个样本，PTS 跨 8.19 s（约 35 fps，72 Hz 隔帧）；`eos: true`；5.0 MB |
| `canvas` 截图 | 2592x1200 `stereo_sbs`：投影层两只眼，每眼按截取尺寸 1296x1200 拷贝 |
| `canvas` 录像 5 s | 提交 186 帧、跳过 7 帧，`eos: true` |

xr 录像中抽出的帧里有状态层（倾斜的面板，内容 GAME FPS 50.3、XR 72/72、EYE 1296x1200），首张截图里没有：状态层每秒最多重绘 4 次，而当时只在请求到来后才挂截取目标，截图那一帧还没拷到。run108 改为整个会话都挂着。

启动和退出的云同步均为 `AppChangeNumber: 18 -> 18`。

### run108：Hades II（2D 大屏），PID 4501

大屏 swapchain 2250x1264，平面屏。主菜单停留 90 s 后，同一局内做 A/B/A（`vr_hud status` 每 5 s 读一次）：

| 阶段 | 游戏 fps | XR fps |
|---|---|---|
| A：不截取（3 次） | 59.9 / 60.1 / 60.1 | 72 / 72 / 72 |
| B：录 `xr` 源（4 次） | 60.1 / 60.1 / 60.1 / 59.8 | 72 / 71.9 / 72 / 72 |
| A：停止后（3 次） | 59.9 / 60.1 / 60.1 | 72 / 72 / 72 |

B 段录像 20.8 s 提交 752 帧（36 fps）、跳过 0 帧，编码 753 个样本，`eos: true`，31.6 MB（约 12 Mbps）。Hades II 主菜单没有用满 GPU，这一组只说明此负载下录像不降帧，不能推到 GPU 吃满的场景。

| 步骤 | 结果 |
|---|---|
| `xr` 截图 | 2592x1200 `stereo_sbs`：头显平放时看到的倾斜大屏，两眼视差正确；状态层在大屏左上方，截图首帧即可见；无环境层处为黑 |
| `canvas` 截图 | 2250x1264 `mono`，大屏原图，无损 |
| `canvas` 录像 5 s | 2250x1264 H.264，`eos: true` |

两轮里 `vkQueuePresentKHR`（向编码器 Surface 送帧，在队列锁内）都没有超过 2 ms 的记录。启动和退出的云同步为 `AppChangeNumber: 7 -> 7`。

## 实时旁观窗口（live 模式）

my_mcp_tools `capture-sdk/tools/liveview.py` 用 `capture_video start_live TOKEN left` 接收 `SCAP1` 流，经 `adb forward` 在电脑上开窗口显示单眼画面（用户级 skill `vr-spectator`）。

- run109（Hades II，APK `4187a427`）：左眼 `xr`，1296x1200，25.1 s 收到 894 个包（约 36 fps）、17.4 MB，跳帧 0；保存的流可完整解码，画面为空间中的大屏加状态层。
- 第一版窗口用 ffmpeg 的 `-f sdl2` 输出：数据正常，但 Windows 报告窗口「未响应」，点击无反应（ffmpeg 7 的多线程 muxer 下 SDL 窗口不处理消息）。改为 PyAV 在工作线程解码、Tk 主线程只负责显示后，本地回放 10 s 解码 892 帧、窗口保持响应；run111（Alyx，APK `f829a744`）实机窗口 `Responding=True`。
- 头显休眠时 app 在后台，DebugBus 服务无法启动（`pid=(not running)`），需要先唤醒。

## 跳帧优化、码率/尺寸设置和全尺寸截图

Alyx 场景内左眼直播在第一版流程下有约 16% 的帧编码不上（run110：621 s 跳过 3752 次）。新增的 `status` 字段显示跳过全部是「编码器没有空闲图像」，我们的合成提交从提交到完成只有 2–3 ms，瓶颈在编码器一侧（`c2.qti.avc.encoder` 是硬件编码器，Surface 至少要 18 张图；日志里能看到它的输入预处理滤镜 `imgTxrF`/`cvpF`）。

同局 A/B/A（run112，各 60 s，`debug.xrgame.capture.legacy` 切换）：

| 段 | 编码帧率 | 跳过 | 合成延迟均值 |
|---|---|---|---|
| A1 原流程 | 29.0 fps | 630 | 2.7 ms |
| B 失败下一帧重试、多一张备用图、单眼只合成一只眼 | 32.2 fps | 453 | 1.9 ms |
| A2 原流程 | 27.7 fps | 501 | 2.7 ms |

同局编码器配置对比（run113，各 45 s，均为新流程）：

| 段 | 编码帧率 | 跳过 | 游戏 fps |
|---|---|---|---|
| P1 默认调度 | 34.8 | 110 | 48–50 |
| P2 实时调度（`priority=0`、`operating-rate`=2×fps、`latency=1`、无 B 帧） | 35.9 | 12 | 47–49 |
| P3 实时调度 + 4 张备用图 | 35.5 | 41 | 48 → 29 |
| P4 默认调度 | 36.0 | 0 | 28–33 |

P3/P4 后段游戏帧率降到约 29 fps（负载下降），只有 P1/P2 在相近负载下可比；实时调度保留为默认。不同会话之间差异较大：run115 默认直播 15.8 s 跳过 144 次，同局 `max_size=720`、4 Mbps、24 fps 的直播跳过 0 次。

`capture_config`（与 scrcpy 的 `--video-bit-rate`/`--max-size`/`--max-fps` 对应）在 run115 验证：默认直播 1296x1200（半眼缓冲），`max_size=720` 时 720x666，结束后恢复为默认。截图改为全尺寸：xr 双眼 5184x2400、单眼 2592x2400，VR `canvas` 2592x1200（投影 swapchain 原尺寸）。APK `4e4f036adffe8385cf0078473e8a15774b4f24e50de2f717f602be6d718bd945`。

## 进游戏前的 2D 窗口（旁观者看游戏库）

没有沉浸式会话时，SteamPSP 的窗口（游戏库、启动面板、对话框）由 `XrPanelCapture` 作为生产者（`steampsp.panel`），同一组 DebugBus 命令和 `liveview.py --follow` 都能用。同一次开机（boot id 同上），运行时 bundle 同上，capture SDK 为 my_mcp_tools `codex/scrcpy-steampsp-capture` 的 drain 改动（见下，提交为 `e6b4d2f`；run117/118 用的是提交前同内容的工作区）。

| 运行 | APK | `libxrimmersive.so` Build ID | PID | 内容 |
|---|---|---|---|---|
| run116 | `9b335d77ee87197a7315d63065c0606b422c71d521fbd13b83746951e17e8e61` | `df8fb9323266aecb32b7134670fd75c749ebe43f` | 22697 | 第一版：HWUI 用 `Surface.lockHardwareCanvas` 直接画进编码器的输入 Surface |
| run117 | `c18d0968a8ff887d4935813d80bf1c5cd43fde43a5cd292a203e2e487e8b09a5` | `7771d1847d2fe6704aa01fc25a5c0433452a5f6d` | 25098 | 改为 `AImageReader` 中转；约 2 分 45 秒 |
| run118 | `59dde4f74b6b15e257c11ff533e59dd6b22d9ffcf630e02c4827b503a17e247d` | `dd8ff1b8a512da21997b3b795f738aa7f71fc534` | 26965 | 加上按理想时间线排帧；约 2 分钟 |

**run116（失败，保留）。** 2D 窗口截图（1920x1080）正常，直播开始后约 7 s 停住，之后整个 app 无响应，DebugBus `launch` 与 `quickmenu exit` 都超时。`debuggerd` 栈：主线程在 `ThreadedRenderer.syncAndDrawFrame` 等 RenderThread，RenderThread 卡在编码器输入 Surface 的 `BufferQueueProducer::dequeueBuffer`；EncoderSession 的 drain 线程已退出，live socket 的写线程还在等包（所以不是订阅端断开）。日志里 `lockHardwareCanvas` 每帧都对编码器 Surface 断开重连、重新分配缓冲（`GraphicBufferSource ... released unpopulated slots`），从 11:40:15.6 起编码器不再取帧，16.3 s 起 `dequeueBuffer` 不再返回。drain 为何退出没能取到（SDK 当时不记录这种退出的原因）。根因在设计：HWUI 的 RenderThread 全进程共用，任何一个它画进的 Surface 卡住，app 所有窗口都会卡住。

**改法。**
- 我们自己的 `HardwareRenderer`（整个录制只 `setSurface` 一次）把窗口画进 `AImageReader`（4 张图）。
- 读取器的回调只保留最新一张。`xr_panel_feed.cpp` 的线程（`XRGame:PanelEnc`）经 GLES blit 把它送进编码器的输入 Surface（`EGL_RECORDABLE_ANDROID`，交换间隔 0），只有这个线程可能等编码器。
- 编码器失败、提前 EOS，或一次投递超过 3 s，就结束录制并报 `failed`。停止编码器会让其 Surface 失效，从而放开卡住的投递。
- SDK 的 drain 现在把 `AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED` 当作信息码，dequeue 出错带上错误码，未请求的 EOS 记为失败，所以不会再无声退出。

**run117（Hades II 1145350）。**

| 步骤 | 结果 |
|---|---|
| 2D 窗口截图 | 1920x1080 PNG，无损 |
| `liveview --follow` 140 s | 4 段流：2D 窗口 → 2D 窗口（启动时 activity 重新 resume）→ `xr_composite` 1296x1200（大屏）→ 退出后回到 2D 窗口；共 4004 个包、解码 3996 帧、103 MB |
| 推流 15 s 时 `capture_video status` | `recording`，提交 351、编码 350、丢弃 1 |
| 推流中 DebugBus `status`、`launch`、`quickmenu exit` | 都及时返回 |
| HWUI 对读取器的连接 | 每段录制一次 connect（run116 是每帧一次） |
| 直播 10 s 后杀掉查看器 | `status: failed`，`error: live capture subscriber disconnected or too slow`；日志 `rendered 254, fed 254, dropped 0, encoded 254`；DebugBus 照常应答，随后的 2D 窗口截图正常 |

**run118（帧节奏，系统 72 Hz）。** 原来按「当前时间 + 间隔」排下一帧，72 Hz 下 30 fps 每次都要等三个 vsync，只有 24 fps。改为沿理想时间线累加，2D 窗口与 XR 会话的 `max_fps` 都按此修正。

| 录制 | 尺寸 | 样本 / 时长 | 帧率 |
|---|---|---|---|
| 2D 窗口（默认 30 fps） | 1920x1080 | 459 / 15.20 s | 30.14 |
| Hades II `xr` 左眼，`max_fps=30` | 1296x1200 | 449 / 14.99 s | 29.88 |
| Hades II `xr` 左眼，默认（隔一个 XR 帧） | 1296x1200 | 364 / 10.06 s | 36.07 |
| 退出后的 2D 窗口 | 1920x1080 | 310 / 10.27 s | 30.09 |

## 旁观窗口的状态面板、声音、GPU 读数，以及下沉到 Foundation

同一次开机，运行时 bundle 同上。

| 运行 | APK | `libxrimmersive.so` Build ID | PID | 内容 |
|---|---|---|---|---|
| run121 | `4a8d021aa17d5cfef096e6704f8765bf42d8c6ed8dab27df8dab8c14c0364dc2` | `f0104c9f6eda5491230829b17c5e704e7c0a594d` | 29940 | 状态层 GPU 读数、失败后重新开始、`vr_hud` 导出格子 |
| run122 | `b94439acbd4cfd4e2133edecb7f78c9f00f86412b1aeed11288e50af8c7efbb7` | `cc123a275503e9530731efcbd2328a9ddf76ea8a` | 13627 | 截取改由 Foundation `modules/capture` 实现后的回归，约 4 分钟 |

- **状态层的 GPU 一栏**一直是 `--`：Swan 处于 SELinux enforcing，app 读不到 `gpu_busy_percentage`、`gpuclk`、`clock_mhz`、`devfreq/cur_freq`（run-as 下 Permission denied），只有 `gpubusy`（`vendor_sysfs_kgsl_gpubusy`）可读。10-12 跳过 `gpubusy` 的做法就让这一栏永远为空。现在改回读 `gpubusy`，偶尔出现的 `0 0` 窗口由 Foundation 的读取器短暂沿用上一值：Hades II 中读到 80–85%，Alyx 菜单中 99%。KGSL 性能计数器也关着（`perfcounter` 为 0），所以头显里拿不到 GPU 频率。PC 旁观面板通过 adb 读 `gpuclk` 补上，标注 `(adb)`。
- **失败后重新开始**：直播因查看器断开而失败后，`wanted` 仍为真，新的 `start_live` 被当成「已在录制」而忽略（revision 停在 5），新开的查看器永远连不上。现在处于 `failed` 时接受新的开始：run121 中 revision 5 → 6，进入 `recording`。
- **跟随面板**：`vr_hud` 的 `cells`（Foundation `StatusCells`）在 run121 Hades II 中给出 10 个格子，`cellsAgeMs` 约 0.1 s，与头显状态层一致。liveview 的面板停靠在窗口右侧，跟随窗口移动；关闭面板会关闭整个查看器（Windows 可能把面板当作主窗口）。
- 查看器的面板、声音和设备名在 my_mcp_tools `83fa1fb`。
- **声音**：查看器旁边运行 scrcpy 4.0 的只传音频模式（`--no-video --audio-source=playback --audio-dup`）。8 s 录音为 Opus 48 kHz 双声道，RMS −42 dBFS、峰值 0.035（Hades II 当时比较安静），说明能抓到播放的声音。头显本身是否仍出声（`--audio-dup`）没有在头显里确认。

**run122（Foundation 下沉后的回归）。** 截取模块搬到 Foundation `d859388`（分支 `feature/malos/capture-producers`；`CaptureHub`、`XrLayerCapture`、`WindowCapture`，见 Foundation `docs/guides/emulator-capture.md`），图层拷贝从图形管线的全屏 blit 改为计算着色器（sRGB 视图采样后再编码回去）：

| 项目 | 结果 |
|---|---|
| 2D 窗口 | 截图 1920x1080；录像 5 s，30.3 fps，丢弃 2 |
| Hades II `xr` / `canvas` 截图 | 5184x2400 `stereo_sbs`（大屏和状态层，双眼）/ 2250x1264 原图，颜色正常 |
| Hades II 录像 6 s | `xr` 左眼 1296x1200 35.8 fps、`canvas` 2250x1264 35.7 fps，跳帧 0；合成延迟均值约 8 ms |
| Hades II `vr_hud` | 游戏 60.0、XR 64.8/72、CPU 33% 2.6 GHz、GPU 81%、温度 72/63 °C 等 10 格 |
| Alyx `xr` / `canvas` 截图（主菜单） | 5184x2400 合成（含状态层）/ 2592x1200 投影两眼原图，颜色正常 |
| Alyx 录像 6 s | `xr` 左眼 23.9 fps、`canvas` 23.3 fps；跳帧 147 / 161，全部是「编码器没还图像」；合成延迟均值 1.7 / 2.2 ms |

Alyx 的跳帧属于之前记录过的编码器一侧瓶颈（同类：run110 场景内 16%）。这次是刚启动后的主菜单，设备在降温节流中（`thermal-engine` 正在设置冷却档位）。没有在同一局里和下沉前的版本对比，所以不能断言 Alyx 录像帧率没有变化。

## 未覆盖与已知限制

- 没有在 GPU 吃满的场景（Alyx 场景内、MHR）测截取的开销；Alyx 只做了功能验证，没有 A/B。
- 键盘图层、Windows VR 的叠加层（快捷菜单）、曲面大屏、开启插帧时的大屏没有专门截到。
- 直通（VST）画面、Pico 系统自己的图层、GLES 合成路径、Windows VR 加载插画（interstitial，只在内容更新时绘制）不在截取内容里。
- Windows VR 的 `canvas` 是按截取单眼尺寸（推荐眼尺寸的一半）拷贝的投影图，不是全分辨率。
- 开始录像时状态沿用上一次录像的 `layout`/`width`/`height`（run108 可见），已改为开始时清空，只编译验证，未上机。
- 2D 窗口只含 SteamPSP 自己的窗口（activity 加对话框），不含 Pico 环境、系统浮层、`SurfaceView` 内容；阴影按平台默认光源近似。只在游戏库和 Hades II 启动/退出上测过，启动面板、设置页、长时间直播未专门测。
- 编码器卡住时（run116 的情形）的自动结束只用杀掉查看器的方式触发过「失败」路径，3 s 不收帧的判定没有实际触发过。
