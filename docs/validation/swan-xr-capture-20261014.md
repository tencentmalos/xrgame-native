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

## 未覆盖与已知限制

- 没有在 GPU 吃满的场景（Alyx 场景内、MHR）测截取的开销；Alyx 只做了功能验证，没有 A/B。
- 键盘图层、Windows VR 的叠加层（快捷菜单）、曲面大屏、开启插帧时的大屏没有专门截到。
- 直通（VST）画面、Pico 系统自己的图层、GLES 合成路径、Windows VR 加载插画（interstitial，只在内容更新时绘制）不在截取内容里。
- Windows VR 的 `canvas` 是按截取单眼尺寸（推荐眼尺寸的一半）拷贝的投影图，不是全分辨率。
- 开始录像时状态沿用上一次录像的 `layout`/`width`/`height`（run108 可见），已改为开始时清空，只编译验证，未上机。
- `live` 模式（`start_live`）未测。
