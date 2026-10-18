@AGENTS.md

`AGENTS.md` holds the shared facts and constraints; this file is only the Claude entry point.

- 2026-10-17 官方 Steam 大屏 GPU 绘制（实验性，默认关，分支 `feature/big_screen_rendering`，未提交）：webhelper 的 CEF GPU 进程和 Steam 合成器改走组件自带的 Zink → glibc Turnip（glvnd 厂商 `steampsp`），每个 Chromium 窗口经宿主 AHB 链在客户端 Android X server 上有自己的覆盖窗口，按 `_XRGAME_STACK_KEY` 保持 Xephyr 中的层级；Mesa 另加 kopper 读回补丁（Xephyr 没有 DRI3）。Odin 3（A8xx）同一 MHW 详情页滚动：26.4 → 41.4 Hz，总 CPU 1.71 → 0.58 核。软件模式的热点是 CEF 自己的软件合成，不是 Steam 合成器。鼠标指针在 GPU 模式下不可见；启动游戏、长时间运行和 Thor 上的 v5 尚未测。同时修复了 X server 的 DRI3/Present 资源泄漏。见[规格 §12](docs/specs/xrgame-native-big-screen-rendering-v1.md)及[记录](docs/validation/big-screen-gpu-20261017.md)。

- 2026-10-17 用户要求游戏下载目录统一：新装使用 `/sdcard/SteamPSP`；旧版已有 `/sdcard/XRGameNative` 游戏库原地复用。Official/Custom Steam 共用同一份 `steamapps` 和现有每游戏容器的存档，不做两套目录的双向复制。

- 2026-10-08 用户指定后续本机编译默认使用 Apple Container：`tools/xrgame/apple-container.sh build`，首次运行 `setup`。源码在 Mac 编辑，Linux ext4 卷保留独立工具链/缓存，产物为 `picoXrDebug` + 原生 `RelWithDebInfo`。见 [操作指南](docs/building/apple-container.md)；完整运行时重建与设备验收不能由 APK 构建推定。
- 2026-10-15 Alyx 的 Vulkan 渲染（`-vulkan`）在 Swan 进场景出画面，游戏自带的 FDM 注视点渲染接上 Pico 眼动。按用户要求“原生路径优先，没办法再做 guest patch”，全部改在原生层：
  - OpenComposite 在 `-vulkan` 时写 `initUsingVulkan=true`；
  - 运行时返回 Vulkan 设备扩展；
  - Wine 补丁 `wine-vulkan-host-image-copy.patch` 在宿主设备上补开 `VK_EXT_host_image_copy`（游戏是 Vulkan 1.0 实例，没启用这个扩展却调用 `vkCopyMemoryToImageEXT`），出包 Proton xrg6；
  - 注视位姿随 views 快照传给运行时，运行时实现 `XR_FB_eye_tracking_social`；
  - OpenComposite 新补丁 `eye-tracked-foveation.patch` 实现 `GetEyeTrackedFoveationCenter`，DLL `54e702ca` 为 MSVC 构建。
  - 调试覆盖 `debug.xrgame.xr.gaze=右,上`（度）验证了 x 数值与 y 方向（游戏用 `vr_fdm_offset_flip_y` 取反 y）。
  - 真人佩戴下的效果、画质和 GPU 开销尚未验收。同日的 guest patch v1 见[规格](docs/specs/xrgame-guest-patch-v1.md)。
  - 见[记录](docs/validation/swan-alyx-vulkan-20261015.md)。
- 2026-10-15 两个调试器按用户要求修复（Alyx FDM/ETFR 改走 guest patch 之前）：
  - guest：WineDbg 改由 DebugBus `guest_debugger` 从 app 进程启动（`run-as` 是 `runas_app`，Swan 的 SELinux 会丢掉 wineserver 的 fd）。WineDbg r7 放行除断点、单步外的全部首次异常。
  - Alyx 附加时约一半会死：Wine 的 `CreateRemoteThread` 线程继承了代为创建它的线程的信号屏蔽字，那个线程常在信号处理函数里等调试器，SIGTRAP 是屏蔽的，break-in 线程第一次 `brk` 就把进程杀了。Wine 补丁 `wine-new-thread-sigmask.patch`，出包 Proton xrg7；同一局 Alyx 附加/分离 8 轮全部存活，MCP 完整流程通过。DebugBus `stop` 改为 SIGKILL。
  - host：native-debugger MCP 在 gdb-remote 附加时没选 Android 平台，信号表编号错位（7 号是 SIGEMT），放行 SIGBUS 无效；已在私有 `my_mcp_tools` 修复并安装本地版 `gdbsig1`。2026-10-16 用 `tcp_gdbserver`（显式端口）对 app 端到端验证通过：45 s 无误停，暂停、继续、分离都正常。默认的 abstract_platform 传输仍未解决，要显式指定 TCP。
  - 2026-10-16 在场景中的 Alyx 上把整条链测了一遍：
    - host：XR 会话中附加，在 `libxrimmersive.so` 下函数断点、单步、分离（XR 90/90），IDA 关联到 `nativeWaitWindowsFrame`。
    - abstract_platform 断开的原因是 CodeLLDB 1.12.1 并行加载约 570 个模块时退出，脚本中关掉 `target.parallel-module-load` 即可。已打进本地包 `gdbsig3`，重连 MCP 后验证。
    - guest：tier0 `Plat_FloatTime` 软件断点命中，删除断点后单步正常（MCP 不允许直接从自己的软件断点单步），IDA 反编译对得上。WineDbg 没有混合栈，用 `read_memory` 读返回地址。
    - patched run-as：经 MCP 环境事务安装（关 verity、重启），新增 `--app-domain`（仅 root：package UID + App 自己的 `untrusted_app` 域），helper 用 `--launcher run-as-app-domain`。`2db6782d` 暂在 `/data/local/tmp`，换进系统要等 gdbsig3 资产。
    - 两种启动方式对比：host lldb 停住 SteamPSP 时 DebugBus 超时，run-as 方式照常启动并命中 guest 断点。guest + host 联调用它，默认仍为 DebugBus。
  - 见[记录](docs/validation/swan-guest-debugger-20261015.md)。
- 2026-10-16 Alyx 眼动注视点的 guest patch 第一轮（`tools/xrgame/guest-patch/games/546560/`，静态分析用 reverse_study MCP）：
  - `etfr_fdm_split`：默认立体模式两眼共用一层时，把游戏的 FDM 图案按每只眼各生成一次；
  - `etfr_offset_eye`：游戏把注视换成偏移时按两眼总宽，补丁改为单眼宽度（x 减半）；
  - `etfr_fdm_ab`：同一局里开关游戏的 FDM，只用于测量。
  - 无人佩戴、用注视覆盖加 Turnip `fdmviz` 测：每只眼的全密度区跟着注视，误差在一个粒度（32 像素）内（右 20°：+208，预期 216）。
  - 游戏自己的“周边发黑”（没打补丁也有）来自景深预通道，启动参数加 `+vr_depth_prepass 0` 就没了。
  - **在这套 Turnip（xrg16，A840）上 FDM 是亏的**：同一局里 FDM 开永远比关慢（27.6 对 51.3 fps；`TU_DEBUG=gmem` 下 25.3 对 38.1；24.6/26.3 对 32.4/40.3），不要默认打开 Alyx 的 ETFR。
    - 均匀全密度的图（没有任何着色节省）更慢（同一局 11.7 对 49.8 fps），所以开销在每个分箱，不在着色。读代码：Turnip 在 A8XX 上没有硬件分箱缩放（`bin_scale_en` 要求 CHIP < A8XX），缩放分箱没有 LRZ，每个分箱都要重放 CPU 生成的 patchpoint（视口、裁剪、FS 参数）加缓存失效和 WFI；FDM 通道还被强制走 GMEM。逐分箱 GPU 时间（u_trace）没量。要修就得改 Turnip，需要用户批准。
    - Swan 的高通系统 Vulkan 驱动有 Alyx 要的能力（Vulkan 1.4、`VK_EXT_fragment_density_map_offset`、`VK_EXT_host_image_copy`），但没有人给它呈现：它是 Android HAL 驱动，v1 不用 `libvulkan_wrapper`，Turnip 的 X11/AHB 呈现是 Mesa WSI。要做的话得在 Wine 的 win32u 里用 AHB 图像自己做软件 swapchain（宿主加载器换成 Android 的 `libvulkan.so`，经 DRI3 交给 X server）。没开始，用户还没选路线。
  - 没测：真人佩戴、`fove_square_use_legacy`、径向图案、长时间游玩。
  - 见[记录](docs/validation/swan-alyx-etfr-guest-patch-20261016.md)。
- 2026-10-02 普通 Android SBS 已在 AYN 显示 Alyx 双眼主菜单与虚拟手；OpenComposite 新 ABI 与 GBE 离线本地通信已修复。双手固定 pose，按键/摇杆从 Android 注入到 Windows OpenXR action 的验证通过。完整游玩、物理手柄与 Swan 尚未验收。见 [SBS 规划](docs/specs/xrgame-native-vr-sbs-v1.md)及[证据](docs/validation/vr-sbs-20261002.md)。
- 2026-10-12 新增普通游戏的「2D 大屏（XR）」显示模式：只在头显上提供，不依赖 `XR_BUILD`，游戏在 OpenXR 会话里以大屏显示。尺寸预设都在 1.5 米焦平面上，分 65 / 80 / 100 / 120 英寸，默认 80 英寸（水平视角约 61°）。XRGAME 新建容器默认 90 Hz。大屏模式使用 Vulkan 合成，显示与 Alyx 相同的状态层。GPU 直通原先没生效，原因是 `VulkanRenderer.setVulkanXrFrameBridge` 在非 `XR_BUILD` 下直接返回；修复后 MHR 导入 1280x720 共享 AHB，Alyx 无人值守进场景回归通过。Pico 的刷新率枚举只列当前显示刷新率，运行时会拒绝其他值，所以只有系统在 90 Hz 时才能拿到 90。Pico 系统资源管理服务（`resmanagerserver`，日志标签 BrainCore）在 Alyx 场景负载下把全系统节流到 72 Hz，之后一直没解除。状态层在大屏模式下与屏幕平行，放在左上角外侧；立体模式下保持竖直，不再朝眼睛偏转。GPU 读数改用 `gpu_busy_percentage`，因为 Swan 的 `gpubusy` 一直为 0。MHR 约 27 fps，受限于自身主线程和每帧 27 ms 的 GPU 工作，频率都被系统限在硬件上限以下。大屏新增 VST 透视（按游戏开关）：Pico 上启动 `XR_FB_passthrough` 后由运行时在背景里画，不提交透视层，混合模式保持 OPAQUE。XR 模式支持外接手柄：XR 手柄和蓝牙 Xbox/DualSense 手柄谁最后按下谁驱动 1 号玩家；长按 Start 600 ms 开快捷菜单；震动跟随当前手柄；状态层 INPUT 一栏显示当前输入。见[记录](docs/validation/swan-xr-bigscreen-20261012.md)。
- 2026-10-14 按用户要求移除 D3D 渲染缩放（含最初的阴影缩放）：不适合 Steam 游戏，移除以降低实现复杂度。实现保留在主仓分支 `feature/malos/d3d-render-scale`（`4d5f0f484`）和 fork 的 `feature/malos/xrgame-resource-scale`，不再维护；容器里残留的 `XRGAME_RENDER_SCALE` 不再被读取。现在组件为 vkd3d xrg4、DXVK xrg7，只含纹理质量。代价：MHR 标题前后少了阴影数组省下的约 1 GiB，当天 MHR 加载时被 Pico lmkd 杀过一次（`POLICY_SWAP_THEN_KILL`）。启动面板同时去掉「插帧」，加上「限帧」（快捷菜单 FPS 限制器的 `fpsLimiterEnabled`/`fpsLimiterTarget`），VR 渲染分辨率选中后显示单眼尺寸，并新增「高」「极高」两档：每轴从推荐尺寸走向运行时最大眼尺寸，Swan 上极高为 4160×3552；Windows 运行时声明的交换链上限由 4096 改为 16384。纹理质量只保留高/中/低选项，不显示说明。见[移除记录](docs/validation/swan-d3d-render-scale-removal-20261014.md)。
- 2026-10-13 转换层新增「纹理质量（高/中/低）」和「阴影渲染缩放（100/75/50%）」（缩放已于 10-14 移除，见上一条），在启动面板设置。vkd3d-proton（组件 xrg2，现 xrg4）和 DXVK（组件 xrg5，现 xrg7）以补丁实现。纹理按 mip 长边上限丢层级（中 1024、低 512），BC 对 BC 不转码；阴影只缩放深度数组。Alyx 的方形深度图会和渲染目标绑在一起，所以不缩放方形图。MHR 低 + 50%：标题时 GPU 内存从 2.9 GiB 降到 2.1 GiB，进标题不再触发 LMK 连杀。Alyx 低：场景内从 3.6 GiB 降到 2.7 GiB。画面对比和「中」档实测尚未完成。见[规格](docs/specs/xrgame-d3d-render-scale-texture-quality-v1.md)及[记录](docs/validation/swan-d3d-texture-quality-20261013.md)。
- 2026-10-14 接入 scrcpy capture SDK（私有 my_mcp_tools，经 Foundation `modules/capture`，只进带 SDK 构建的内部 debug 包）：DebugBus 新增 `capture_source`/`capture_screenshot`/`capture_video`，用 SDK 的 `key: value` 格式回答，`capturectl.py --app steampsp` 和 scrcpy MCP（`service` 填 SteamPSP 的 DebugBusService）可直接用。`canvas` 为大屏图（mono）或 VR 两眼投影图；`xr` 把会话提交的图层按眼合成、无镜头畸变（单眼为推荐尺寸的一半）。参考了 GFXReconstruct 的 OpenXR 层：它录 API 不录像素，且依赖自己的 Vulkan 层，而合成设备经 adrenotools 绕过 loader，故只借用「release 前拷贝」和按层位姿合成的做法。状态层/键盘靠 Foundation `XrImguiVulkanLayer` 新增的截取目标（分支 `feature/malos/xrgame-imgui-capture`，`1e7d6f0`）。Swan 上 Alyx、Hades II 两种源的 PNG 与 H.264（72 Hz 下 36 fps）通过；Hades II 同局 A/B 录像时仍 60 fps / 72 Hz（非 GPU 瓶颈）。没有沉浸式会话时，同一组命令截取 SteamPSP 自己的窗口和对话框（`steampsp.panel`）：由我们的 `HardwareRenderer` 画进 `AImageReader`，再由原生线程送进编码器；直接让 HWUI 画进编码器 Surface 会在编码器停收帧时卡死整个 app（RenderThread 全进程共用）。同日把截取下沉到 Foundation `modules/capture`（`CaptureHub`、`XrLayerCapture`、`WindowCapture`；图层拷贝改为计算着色器），附其他模拟器的接入指南（Foundation `docs/guides/emulator-capture.md`）。xrgame 的 `xr_capture.cpp` 只做适配。`vr_hud` 增加状态层格子（Foundation `StatusCells`），供 PC 旁观窗口的跟随面板显示。状态层 GPU 改回读 `gpubusy`：SELinux enforcing 下 app 读不到 `gpu_busy_percentage` 和任何 GPU 频率节点。录制失败后不再挡住下一次开始。见[记录](docs/validation/swan-xr-capture-20261014.md)。
- 2026-10-14 Alyx 云存档再次反复冲突：出站白名单拦了新的存储主机导致一批上传半途失败，之后 `DuplicateRequest`（0 块）被当失败，云端变更号每批增长、本地号不再前进。已加用户批准的 `steamcloud-sg…` 主机；重复项在云端清单 SHA 一致时算已同步；XRGAME「落后云端且本地有改动」先按 SHA 比对云端（内容相同采纳云端号，云端仍等于上次同步则上传）。单测通过，Swan 启动/退出同步为 `18 -> 18`。同日补充：
  - 第一次假冲突来自一批部分失败的上传：9 个文件里 8 个已提交，1 个被白名单拦下。Steam 会保留失败批次里已提交的文件，云端变更号照样前进，我们这边却什么都没记。
  - 现在批次失败后会重读完整的云端清单。云端已有同 SHA 的上传、`ClientDeleteFile` 确认过的删除计入缓存。只有合并结果与云端完全一致时，才记下清单的变更号；失败的文件仍算本地改动。
  - 上传失败后不再补下载从未同步的文件，以免把没传上去的改动记成已同步、日后被下载覆盖。
  - DebugBus `quickmenu exit` 改为直接执行「退出」项的动作。原来合成的确认键会送到持有焦点的 View（游戏中是 TouchpadView），有 3 次没有到达菜单。
  - 见[记录](docs/validation/swan-quickmenu-exit-cloud-batch-20261014.md)。
- 2026-10-13 状态层位置调整（10-14 修正仰角：Swan 上下视场 ±43°，原来的 35° 平视仍在视野内；现按运行时视场放在视野上沿外 4°，Swan 上为 49.3°）：立体模式改为距头部 1.0 m、水平居中、仰角 35°（0.8×0.08 m，视角大小不变，仍保持竖直），平视在视野上沿外、抬头可见，避免场景物体挡在前面造成辐辏冲突。2D 大屏的间隙加大；曲面屏时沿圆弧摆放，并保证面板两端不低于圆弧上沿。见[记录](docs/validation/swan-status-layer-placement-20261013.md)。
- 2026-10-13 2D 大屏接入 Foundation `lossless_fg` 插帧（容器 `XRGAME_XR_FRAMEGEN=off|2x|3x|4x`，DebugBus `vr_upscale fg=`；2026-10-14 按用户要求从启动面板移除「插帧」，改为显示快捷菜单的 FPS 限制器「限帧」）。X server 的 XR 目标改为 5 块 AHB 的环（`xr_frame_exchange.h`），fence 完成后发布；渲染器不等会话，没有空闲 slot 时跳过重试。插帧直接采样 slot（真实帧不拷贝），在 Wine 屏幕尺寸上生成，生成帧与真实帧走同一次超分。MHR loading 段同局对比：游戏 30 fps 不变，大屏 60 fps（72 Hz 下 2x）。场景内画质与 GPU 开销未验收。见[记录](docs/validation/swan-xr-framegen-20261013.md)。
- 2026-10-12 Swan 的 XR 显示应为 **90 Hz**（用户确认）。10-10 的各项测量是在 72 Hz 下做的：同一局里的对比结论成立，但绝对帧率和 GPU 占比不能直接沿用。`dc877ba12` 换成 Clang 版 OpenComposite `f5a979e0` 后，运行时校验没有同步，会导致进 VR 失败，已修复；该 DLL 已在 Swan Alyx 上通过 VR 验证。MHR 已从 Odin3 拷到 Swan，SHA-256 全部一致。见[记录](docs/validation/swan-oc-clang-vr-20261012.md)。
- 2026-10-10 Swan GPU 测量必须在游戏场景里（读档并恢复后）进行，主菜单和暂停状态的数据不算数；对比要在同一局里切换。OpenComposite 原来每只眼都整张拷贝 Alyx 的 3670x1699 双眼纹理，现在只拷显示区域（`displayed-bounds-copy.patch`，DLL `8a8b65ea`）。同局对比：36.0 → 38.3 fps，游戏每帧 GPU 时间 21.9 → 20.3 ms。触发文件 `C:\gamenative-xr\opencomposite-full-copy` 可恢复整张拷贝用于对比，测完删掉。见[记录](docs/validation/swan-oc-bounded-copy-20261010.md)。
- 2026-10-10 DebugBus 可以无人值守地进 Alyx 场景：`launch 546560 load=s0/autosave` 直接读档，`game_state` 读 `-condebug` 控制台日志判断是否已进场景，`input axis rt 1 300` 解除读档后的暂停，`vr_oc full_copy=0|1` 切换 OpenComposite 拷贝方式，`quickmenu exit` 走菜单退出。Swan 上已完整跑通一轮。见 `docs/debugging/debugbus.md`。
- 2026-10-10 SGSR 改为直接重建到 swapchain 层（Foundation `RenderTo`，省掉准备 pass 和输出拷贝）。同局对比：每个新帧的合成耗时 3.22 → 1.30 ms，帧率 27.8 → 30.0 fps。调试开关 `debug.xrgame.xr.upscale.copy=1` 恢复旧路径，`debug.xrgame.xr.upscaleprobe=1` 运行字节等价探针。见[记录](docs/validation/swan-upscale-direct-20261010.md)。
- 2026-10-10 Swan Steam 云存档反复冲突已定位并修复（picoXr）：
  - 删除只在批次里声明、从未真正执行，导致 Alyx 的关卡过渡文件被反复下载和删除，每个批次都推高云端变更号。
  - 云端已有相同内容时，上传返回 0 块、提交报告未提交，被我们的严格判定当成了 UpdateFail。
  - 修复：`XrGameCloudRpc` 记录 EResult，真正执行删除（带护栏），0 块按已同步处理。UpdateFail 时可以「保留本地存档并启动」。
  - 进 VR 时自动收起游戏库主面板，退出后恢复。
  - 已挑合上游 3 个 Steam 提交，暂不整体合并上游。
  - 见[记录](docs/validation/swan-steam-cloud-sync-20261010.md)。
- 2026-10-04 图形 API 录制回放：picoXr debug 包用 GFXReconstruct 录制，D3D12 在 API 层录，其他 API 录 DXVK 转换后的 Vulkan；通过 DebugBus `api_capture` 触发，在 Windows 上脱离游戏回放。AYN 上录的 MHR D3D12 帧在 Windows 原生 D3D12 上回放正确。Wine/FEX 下 D3D12 必须用 unassisted 内存跟踪，用 page_guard 会黑屏。回放过程的 RenderDoc capture 能在 renderdoc MCP 中打开；先选定事件、ID 用十进制字符串传入后，图像导出和 draw 检查可用。见 [规格](docs/specs/xrgame-native-api-replay-v1.md)及[证据](docs/validation/api-replay-20261004.md)；Vulkan 层模式、MHW 和其余 MCP D3D12 工具尚未验证。
- 2026-10-09 Swan：
  - Vulkan 合成 + SGSR 超分 + 眼动 FDM（ETFR）在 Alyx 上生效；
  - Turnip xrg12（mesa `malos/main` `04e1d665`，A8XX 默认关闭 LRZ fast clear）在完整 LRZ 下单次约 19 分钟无硬件卡死，尚未做长时间验收；
  - 合成器 GPU page fault 已于 2026-10-10 修复：Turnip 在 A840 上不初始化 bin foveation 寄存器，继承了其他上下文留下的值。复位提交为 mesa `malos/main` `adb7e30a`，出包 Turnip xrg13，游戏进程和 app 进程都已加载；fault 为 0，全设备 300 s 为 0。全 LRZ 长时间游玩尚未在 xrg13 上测试。见[根因记录](docs/validation/swan-xr-composite-faults-20261010.md)；
  - Swan 手柄射线默认修正 pitch +24°，尚待确认。
  - 游戏设置在 XRGAME 构建中默认显示精简页，完整配置在「Legacy 设置…」。
  - 见[记录](docs/validation/swan-xr-composite-20261009.md)。GPU hang 按最高优先级处理，要修复根因，不靠开关绕开。
- 产品显示名已于 2026-10-07 更名为 **SteamPSP**（PICO Space Pro）；图标与维护说明见 [品牌资源](docs/branding/README.md)。包名、签名、存储目录和代码标识保持稳定，历史记录不改名。
- 2026-10-14 新增本仓 skill `swan-memory-reclaim`（`.claude/skills/`）：`tools/xrgame/swan-memory-reclaim.py --serial <设备>` 查看 Swan 内存，并以 root 执行 `sync; echo 2 > /proc/sys/vm/drop_caches` 清空 dmabuf 空闲池（空闲时约 1–1.5 GiB），宜在大游戏启动前执行；`--status` 只看不回收。用户可能正在玩，脚本会先列出 Wine 进程。
- Default to Chinese when talking with the user.
- **主干**（2026-10-09 用户确认，统一记录）：主仓 `tencentmalos/xrgame-native` 为 `malos/main`；Foundation `tencentmalos/foundation` 为 `main`。Foundation 远端默认分支就是 `main`，没有 `master`。
- 修改 Foundation 前必须从当前固定提交切出独立功能分支，不能直接在共享 `main` 上修改，完成后以 `main` 为合并目标（用户补充，2026-09-27）。
- 用户于 2026-09-27 授权将私有 `tencentmalos/foundation` 作为 `foundation/` 子仓接入 DebugBus，替代原先的禁止引入规则。操作与边界见 `docs/debugging/debugbus.md`，不公开发布 Foundation 源码。2026-10-09 用户放开范围：本项目同为学习研究性质，Foundation 的任何模块（upscale、foveation、fsr1、xr 眼动、lite-engine 等）都可编进内部 picoXr 包；仍以子仓链接，不把其源码拷进本公开仓库，APK 只在内部分发。
- **主仓主干为 `malos/main`**：`tencentmalos/xrgame-native` 的功能分支默认从最新 `malos/main` 创建，并以它作为合并 / PR 目标。子仓分支遵循各自约定；commit / push 仍需用户明确授权。
- MHW 的 Steam/loader/根目录配置路径启动阻塞已修复，AYN 到达 3D 标题画面、开场剧情和角色创建；已观察用户进入冰原场景；受控 DX11 → DX12 → DX11 重启、原 3 个角色识别和正常退出已通过，长时间稳定性仍待验。重启链路见 `docs/validation/mhw-restart-20260927.md`。目录别名仅为 MHW profile 启用，不修改游戏文件，不放宽零长度 D3D buffer。见 `docs/validation/mhw-startup-20260927.md`。
- picoXr 必须内置完整运行时（含源码 Steam 客户端、DXVK 与 VKD3D），缺组件禁止出包。x64 Steam 游戏默认配置客户端，旧容器启动时校验并修复运行库；不修改游戏目录或存档。验证范围见 `docs/validation/bundled-runtime-20260927.md`。
- **Turnip 子仓主干也为 `malos/main`**：已从 shadPS4 的 `codex/turnip-xr-fdm2`（`d15b7c01`）建立并推送；迭代分支 `feature/malos/xrgame-wine-icd` 从该基线切出，不能从 Mesa 上游 `main` 起步。2026-09-28 阶段版本已先推子仓再更新 gitlink；已验证运行时仍采用配方的固定基线与补丁，不能把维护分支 rebase 当成新二进制验收。
- Start from [docs/specs/xrgame-native-personal-v2.md](docs/specs/xrgame-native-personal-v2.md). The user revised the plan on 2026-09-26: personal learning, AYN validation now, Swan deferred; the old WP3 engineering/publication remainder is the final optional P5 and does not block runtime work.
- 2026-09-28 新增 [Mac 独立构建规格](docs/specs/xrgame-native-macos-build-v1.md)：用户同日再次明确暂停 Mac 构建迁移（ROI 低）；保留评估，当前优先将 Present fence 等待移出 X 请求线程及全局锁，并做 MHW 对照验证。当前只完成评估和小型编译探针，未迁移完整运行时或授权公开上传二进制。 异步 Present 改造与 MHW 同包对照见 [验证记录](docs/validation/mhw-present-async-20260928.md)，当前按容器显式启用，不视为全游戏/完整生命周期验收。 图形设置新增“异步呈现（实验性）”，MHR 同包对照与 UI/暂停验证见 [MHR 记录](docs/validation/mhr-present-async-20260929.md)。
- Retain the original v1 acceptance gaps; see [the original-spec/observability review](docs/validation/spec-observability-review-20260926.md). D0–D4 expands guest/host debugging and analysis alongside P1–P3. Do not treat shared FEXCore code as an already integrated Wine debugger, or AYN validation as Swan/XR acceptance.
  - Start with P1 (former WP4 on AYN), using the existing embedded runtime and Steam-installed games; complete the remaining Steam usability checks in P2.
  - Do not split work packages into micro-specs.
  - Measure AYN behavior before adding a shim. Swan's Android 16 matrix and XR capability probe wait for P4; do not treat Android 13 plus targetSdk 36 as Android 16 evidence. Measure before replacing the display path in P3.
- Before changing anything under `references/`, read that checkout's own instructions (for example FEX's AI-contribution ban).

- 新引入的直接依赖仓库先 fork 到 `tencentmalos` 再维护；迁移 URL 不升级 gitlink。GBE、Proton Wine、JavaSteam、GFXReconstruct 等已补齐 fork。

- 调试流程：`tools/xrgame/README.md` 的 Device debugging。native app 与 Wine x64 guest 的基础暂停/读取/恢复/清理已验证，见 `docs/validation/wp3-debuggers-20260926.md`；Hades II 已通过 Turnip AHardwareBuffer/X11 路径进入实际场景并响应移动（GPU 拷贝，无 CPU 读回，保留 present-wait）；完整游戏与 WP3 仍未验收。

- 当前 AYN 的下载/续传/修复/目录识别和独立公共 depot 对照见 `docs/validation/personal-p2-downloads-20260927.md`；guest 软件断点可命中但恢复仍失败，默认关闭，不能把基础 attach/inspect 通过扩大为完整 D2。

- **Steam client modes (2026-10-17):** Official Steam owns its login/download/game session/Cloud through the Linux client and Proton bridge; disconnect and suppress JavaSteam while it runs, including Android game-host Activity entry and reconnect callbacks. Custom Steam remains the original JavaSteam + bundled GBE route. Switch managers only after the previous client/game stops; share physical game/save paths, not two copies. Cached metadata access must not require a second login. See [the sharing record](docs/validation/steam-shared-library-20261017.md). 2026-10-17 Odin 3 v12/v13：官方模式启动 MHW 时超出 Android 的幻影进程上限（默认 32），系统杀掉 PRoot 里整套 Steam，游戏随之退出，与启动信息来源无关；用户自己关闭子进程监控（`settings_enable_monitor_phantom_procs=false`）后，MHW 进到选择存档并开始加载，Steam 进程完好。Custom 模式不受影响。用户决定暂时用 JavaSteam（Custom）模式；大屏（官方模式）启动游戏也暂时沿用之前耦合 JavaSteam 的启动信息（已验证能进游戏、领奖励），桥接传回的 Steam 命令只在 DebugBus `steamCommand` 显示。见[大屏记录](docs/validation/big-screen-gpu-20261017.md)。
