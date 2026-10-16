# Swan：Half-Life: Alyx 的 Vulkan 渲染与眼动注视点（ETFR）（2026-10-15）

目标：让 Alyx 主场景用游戏自带的 FDM 注视点渲染，并跟随 Pico 眼动。

用户先确认走 Vulkan 路线，随后要求“原生路径能不 guest patch 先不 guest patch”。所以本记录里的修复都在 Wine、OpenXR 运行时、OpenComposite 和 app 里完成，没有改游戏代码。

## 背景

- Alyx 加 `-vulkan` 时用 Source 2 的 Vulkan 渲染器。它自带 FDM 注视点渲染（`vr_fdm`，`fove_*`）。眼动偏移走 `VK_EXT_fragment_density_map_offset`。
- 偏移中心来自 OpenVR 的 `IVRSystem::GetEyeTrackedFoveationCenter`（虚表 0x110），对应 Steam Frame 那条路径。我们的 OpenComposite 原来固定返回 false，所以游戏日志一直是 `bGotEyeTracking: false`。
- D3D11 是 Alyx 的默认渲染器，它不走这条路径。

## 进入 Vulkan 场景需要的三处原生修复

1. **OpenComposite 需要 `initUsingVulkan=true`**，否则在 `XrBackend::VkGetPhysicalDevice` 中止（报 “Not using temporary Vulkan instance”）。
   - `WindowsVrPayloadManager` 在 `execArgs` 含 `-vulkan` 时，在 `openvr_api.dll` 旁写入带 SteamPSP 标记头的 `opencomposite.ini`，会话结束时删除。
   - 游戏自带的同名 ini（没有我们的标记头）保持不动。
2. **运行时的 `xrGetVulkanDeviceExtensionsKHR` 原来返回空列表。** 游戏设备因此没有外部内存扩展，`getMemoryFd` 为 NULL，`xrCreateSession` 失败。现在返回 `VK_KHR_external_memory_win32 VK_KHR_external_fence_win32`，Wine 再映射为宿主的 fd 扩展。
3. **host image copy 崩溃。** 进场景后第一次上传 CPU 纹理（日志 “Uploading CPU Texture!”）时，Wine 报 `vkCopyMemoryToImageEXT Exception 0xc0000005 in Unix call`（pc=0），随后游戏退出。用 `trace+vulkan` 抓到的事实：
   - 游戏实例的 API 版本是 1.0（`vkCreateInstance API version 0x400000`）。
   - 游戏设备启用了 24 个扩展，其中没有 `VK_EXT_host_image_copy`；控制台也没有打印 “Yaay! VK_EXT_host_image_copy is enabled”。
   - 但游戏用 `vkGetInstanceProcAddr` 取到了 `vkCopyMemoryToImageEXT` 并调用。Windows 驱动不管扩展是否启用都会返回这个函数；Mesa 会返回 NULL，Turnip 本身支持该扩展。
   - **第一次尝试失败（保留记录）：** `wine-vulkan-promoted-alias.patch` 让 win32u 在找不到 `…EXT` 时改取核心名 `vkCopyMemoryToImage`。APK `1a7073cbba51a04b4139d229c6e6258d08eccc89a470c5bf613629a3c3d6ff93`，Proton 包 `cd3fb973…`。仍然在同一处 pc=0 崩溃，原因是 Mesa 只在实例 API ≥ 1.4 时才给出 1.4 核心入口。该补丁已删除。
   - **修复：** `tools/xrgame/patches/wine-vulkan-host-image-copy.patch`。只要驱动支持，就在宿主设备上额外启用 `VK_EXT_host_image_copy`，连同 `VK_KHR_copy_commands2`、`VK_KHR_format_feature_flags2`，并挂上 `hostImageCopy` 特性结构。若应用自己已挂 `VkPhysicalDeviceVulkan14Features` 或 host image copy 特性结构，则不改动。Turnip 只对带 `HOST_TRANSFER` 用途的图像改变布局，所以其他图像不受影响。
   - 打包：Proton `proton-11.0-2-arm64ec-xrg6`（组件 id 不变），用 `tools/xrgame/package-proton-validation.py` 从 xrg5 替换 `lib/wine/aarch64-unix/win32u.so`。逐成员对比：只有该文件和 `xrgame-build.json` 不同。

结果：Alyx 的 Vulkan 渲染进入 `a1_intro_world` 并稳定出画面（截图 `alyx-vulkan-hic-xr.png`）。状态层显示 GAME FPS 36.0、XR 72 Hz、单眼 1296x1200、GPU 77%；FDM 为 `81x38x1 (RT: 2592x1200x1)`。

## ETFR 链路

| 层 | 改动 |
|---|---|
| app 原生（`xr_immersive.cpp`） | `locateGaze()` 不再只在 Vulkan 合成下运行。Windows 快照携带注视位姿：与 views 同一追踪空间、同一时刻；预测时刻不同时按 views 的时刻重新定位。 |
| JNI / Kotlin | views 数组从 22 个浮点扩到 30 个（有效标志、方向、位置）。`LOCATE_VIEWS` 行末追加具名字段 `ev`、`eq?`、`ep?`；按位置读取的旧解析不受影响。FRAME_SYNC 总长仍在 Wine 侧 2048 字节上限内。 |
| Windows OpenXR 运行时 | 实现 `XR_FB_eye_tracking_social`：声明扩展，`XrSystemEyeTrackingPropertiesFB`，以及 `xrCreateEyeTrackerFB`、`xrDestroyEyeTrackerFB`、`xrGetEyeGazesFB`。两眼方向相同，位置分别取各眼的位置。 |
| OpenComposite | 新补丁 `references/opencomposite/patches/eye-tracked-foveation.patch`（需 `-EnableSystem026`）。可用时启用该扩展，并在每个会话首次调用时建 eye tracker（OC 会为游戏的图形 API 重建会话）。`GetEyeTrackedFoveationCenter` 把各眼的注视方向换算成该眼视锥的 NDC（y 向上，与 `GetProjectionMatrix` 一致）；`…ForProjection` 用调用方给的矩阵。OC 自带的 OpenXR SDK 是 1.0.12，没有这个扩展，所以补丁带了一份注册表定义 `RuntimeExtensions/XR_FB_eye_tracking_social.h`。 |

- 校验：五个 OC 补丁按 `build.ps1` 的顺序打在干净的上游 `a27e7e6` 上都能应用，结果与构建树逐文件一致。
- DLL 用本机既有的 MSVC 19.51.36241.0 构建目录编译（与 `8a8b65ea` 同一目录）。

只在 `ORIENTATION_TRACKED` 时转发注视：
- app 给 Foundation 眼动追踪器的 `locateSpace` 是自己包的一层，用来记录最近一次定位的 flags。这样不改 Foundation，也能区分真正的追踪和运行时给的静止方向。
- 实测 Pico 在头显没人戴、自身 ET 报 `valid=false` 时，flags 仍是 `0xf`。所以在 Pico 上这一位分不出来，这一检查只对其他运行时有意义。

调试开关：`debug.xrgame.xr.gaze=<右>,<上>`（单位：度，相对头部正前方）。设置后，给 Windows 游戏的注视被替换成固定方向，不需要有人佩戴。每 72 帧读取一次；清空后恢复使用头显数据。

### 结果（头显放在桌上，无人佩戴）

| 条件 | 游戏日志 `Foveation Center`（两眼） | `FDM Offsets` |
|---|---|---|
| 修复前 | `bGotEyeTracking: false`，[0, 0] | [0, 0] |
| 无覆盖（Pico 静止方向） | [0.0802, -0.0001]，持续约 12 s 基本不变 | [104, 0] |
| 覆盖 右 20°、上 10° | [0.333529, -0.201231] | [432, -120] |
| 覆盖 左 20°、下 10° | 对称 | [-432, 120] |

Swan 单眼 FOV 为 ±47.5°/±43°。预期 NDC：x = tan20°/tan47.5° = 0.3335；y = tan10°/(cos20°·tan43°) = 0.2012。实测 x 与预期一致。

y 被取反来自游戏自己：反汇编 `rendersystemvulkan.dll`，在调用虚表 0x110 之后，若 `vr_fdm_offset_flip_y` 非零就把 y 取反，再按渲染区域换成像素偏移。日志里取反了，说明该开关默认开启，也就是游戏按 OpenVR 的 y 向上 NDC 读入，再转为 Vulkan 的 y 向下。向上看对应向上偏移 −120 像素，方向正确。

### 未验证

- **真人佩戴时**：注视移动、清晰区域位置，以及偏移量是否与画面一致。游戏按 `Render Area 2592x1200`（两眼并排的宽度）换算像素，是否需要按单眼宽度，要靠佩戴观察确认。
- **画质与 GPU 开销**：在暗处门前的静止画面里，左右两种覆盖下逐块细节（|Laplacian| 均值）差异在 ±4% 内，这个场景细节太少，测不出 FDM 的效果。
- Pico 眼动平台钩子返回 `-1000710000`；有人佩戴时 ET 是否开启待查。2026-10-09 本仓合成器的 ETFR 记录了 ET 有效。
- 没有做长时间游玩，也没有和 D3D11 默认路径比较性能。

## 构建与设备

| 项目 | 值 |
|---|---|
| APK（最终，`picoXrDebug`，含 capture SDK） | `85d4e198523b619d5f670e8055557e129a6d1273d6d2ee981f5852cdd01b6f99`，打包检查 132 项、0 错误 |
| 中间 APK | `3f39b590…`（只有 Wine 修复，Vulkan 场景通过），`9dfeb598…`（ETFR，未按 TRACKED 过滤），`0aaa8b1f…`（加 TRACKED 过滤） |
| `libxrimmersive.so` Build ID | `acb23907a7dc4540aa78c56d17706b141df8b766` |
| `gamenative_openxr_runtime64.dll` | `4622eae70da734671917df903dd505e17b7c4b530ce0ac94d95c8ffc62d21496` |
| `opencomposite_x64.dll` | `54e702ca083f8a88f6fbd794406ba0e8dade343392967bfb882edd90d6c3be06`（MSVC，五个补丁；pin 已更新） |
| `xrgame_guest_patch64.dll` | `b8fb5f4e4ef11e21316cdf01749a27522a4d1b57f8ad35be86889b1e185ba7ec` |
| Proton | `proton-11.0-2-arm64ec-xrg6.wcp` `2c421539badc8692175a8e9a3c2e2eee6014816c156ee5aa48cbc2d8e13435a0`；`win32u.so`（strip 后）`12f6eca3b57b81ffbbf1db6d2ba2930b9c4fa5760d7c0daa8dc144cb60bd5d79` |
| 组件清单 | `38133a710582844911a2c11f5c077f8324b753d4887623183bddd4b82cf0c627`（texq 包组，Proton 换成 xrg6；Turnip xrg13、DXVK xrg7、VKD3D xrg4 不变） |
| 设备 | B3110，`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，boot id `6d4335a5-667c-426d-815b-1a82edaa0fbb` |
| 进程与时长 | 最终一轮 app PID 20721。15:30 启动，15:38 通过快捷菜单退出，退出时云同步正常。 |

## 当前 Alyx 容器状态

- `execArgs` 为 `-condebug -vulkan +vr_fdm 1`。已去掉每帧打印的 `+vr_fdm_offset_debug 1`：它一局就把 `console.log` 写到了 72 MB。
- `WINEDEBUG=warn+seh,warn+module,+loaddll`。
- 仍选着两个 guest patch 探针（`exe_entry_probe`、`client_dllmain_probe`），是否保留由用户决定。
- 去掉 `-vulkan` 就回到 D3D11 路径，ETFR 不生效。
