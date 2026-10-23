# 官方 Steam 大屏：硬件绘制与 OpenXR 大屏路线 v1

日期：2026-10-17。状态：**DRAFT / 路线规划，未实施，未上机**。分支 `feature/big_screen_rendering`，基于 `malos/main` `8c15fe11`；DroidDeck 对照固定在 `references/DroidDeck` `2503d6929332de2a4a22c8a31c1b3ce84e5a8831`。

本文的“大屏”指官方 Linux Steam 客户端的 Big Picture（`-gamepadui`）界面。它与游戏的“2D 大屏（XR）”模式不是同一条显示链。目标有两个：一是把客户端从当前的 CPU 软件绘制改成 GPU 绘制；二是让 Big Picture 将来能直接在 OpenXR 会话里显示。Windows 游戏的运行与显示链不在本文改动范围内。

前置记录：[官方 Linux Steam 接入](../validation/linux-steam-20261016.md)、[共享库](../validation/steam-shared-library-20261017.md)、[DroidDeck 对照](../background/droiddeck-comparison-20261013.md)、[AHB 减拷贝评估](xrgame-native-ahb-scanout-evaluation-v1.md)、[Swan 2D 大屏（XR）](../validation/swan-xr-bigscreen-20261012.md)。

## 1. 结论

1. **15 Hz 只是问题基线，瓶颈尚未拆分。** 现有唯一数据来自 v17 的一次采样：16.31 s 内 SurfaceFlinger 收到 248 次呈现，约 15.2 Hz，帧间隔中位数 66.89 ms。源码上看，这条链路同时存在 CPU 光栅、socket 传图、单线程 X 请求处理、整帧上传和渲染持锁几处可疑点（§2）。**先做 G0 测量，再决定先做软件链优化还是直接走 GPU。**
2. **DroidDeck 的加速不是加一个 CEF 参数就能得到的。** 它的链路是：glibc 版 Turnip 直接驱动 KGSL，GL 走 Zink，gamescope 用 Wayland backend，app 内自带一个 Wayland compositor 导入 dma-buf，最后一次 GPU blit 上屏。要让这条链跑起来，还需要伪造 render node、DRM GEM/PRIME shim 和伪造 `/proc/bus/pci/devices`（§3）。Big Picture 默认并不走它的 AHB 零拷贝分支。
3. **推荐路线 R-B：保留我们的 Java X server 作为客户端显示服务器。** 客户端 userspace 加入从源码构建的 glibc Turnip（KGSL）。CEF 改用 GPU，经 DRI3/Present 把帧交给我们的 X server，宿主侧走与游戏相同的 AHB/VulkanRenderer 链，再接入现有合成与 XR 帧桥。不引入 gamescope，也不引入 Wayland compositor（§5）。2026-10-17 用户确认按此路线、复用游戏 X server 的方式落地，实际选择见 §11：Xephyr 仍承担 X11 协议，只把 GPU 帧绕过 Xephyr（B-0）；buffer 改由宿主分配（I-3'），宿主侧不需要导入 dma-buf。
4. **OpenXR 大屏与 GPU 化可以并行推进。** XR 合成器只消费 AHB ring（`XrFrameExchange`），不关心像素来自哪里。只要把客户端页面从 GLRenderer 切到 VulkanRenderer（这同时是 GPU Present 的前置），再把 `ImmersiveXrActivity` 与 Wine 游戏容器的耦合抽成“面板来源”，软件绘制的 Big Picture 就能先上头显；GPU 化完成后，XR 侧自动受益（§6）。

## 2. 当前客户端显示链（源码事实）

```text
steamwebhelper (CEF, -cef-disable-gpu)        steam 主进程 (VGUI 需要 GLX)
  Skia CPU 光栅                 [CPU]           llvmpipe, LP_NUM_THREADS=2 [CPU]
  X11 软件输出 → :1             [CPU]
                    ▼
Xephyr :1（fb，无 glamor，-sw-cursor）
  损坏区域 → PutImage 经 /tmp/.X11-unix/X0   [CPU + 内核拷贝；无 MIT-SHM]
                    ▼
Java XServer（单个 XConnectorEpoll 线程）
  PUT_IMAGE 持 DRAWABLE_MANAGER → Drawable.drawImage → JNI memcpy 到 BGRA ByteBuffer [CPU]
  → forceUpdate → GLRenderer.requestRender
                    ▼
GLSurfaceView 线程（RENDERMODE_WHEN_DIRTY）
  持 DRAWABLE_MANAGER → 整个 drawable glTexSubImage2D(GL_BGRA)  [CPU→GPU]
  → 四边形 + 光标 → eglSwapBuffers → SurfaceFlinger
```

| 事实 | 位置 |
|---|---|
| Steam 参数是 `-gamepadui … -cef-ozone-platform=x11 -cef-disable-gpu` | [LinuxSteamService.kt](../../app/src/picoXr/java/app/gamenative/xrgame/steam/LinuxSteamService.kt) L161–163 |
| 客户端专用 X server 是 `XServer(ScreenInfo(w,h), true, false, 24)`，移除了 MIT-SHM、SYNC、DRI3、Present | 同上 L126–152 |
| Xephyr 与 Steam 都设 `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2`；Xephyr 无 glamor | [launch-client.py](../../tools/xrgame/linux-steam/launch-client.py) L44、L63–67 |
| 客户端页面用 `XServerViewGL`（GLRenderer） | [LinuxSteamActivity.kt](../../app/src/picoXr/java/app/gamenative/xrgame/steam/LinuxSteamActivity.kt) L101–102 |
| 每次更新都整帧 `glTexSubImage2D`，不按损坏区域 | [Texture.java](../../app/src/main/java/com/winlator/renderer/Texture.java) L62–75 |
| GL 线程上传和绘制期间一直持 `DRAWABLE_MANAGER`，与 X 请求线程上的 PutImage 争用同一把锁 | [GLRenderer.java](../../app/src/main/java/com/winlator/renderer/GLRenderer.java) L333–337 |
| Present 的 GPU 分支要求 VulkanRenderer；GL 下只能回退 CPU `copyArea` | [PresentExtension.java](../../app/src/main/java/com/winlator/xserver/extensions/PresentExtension.java) L436 |
| userspace 只有 GL/EGL/GBM（Ubuntu 软件 Mesa），没有 Vulkan loader、ICD 或 glibc Turnip | [build-rootfs.sh](../../tools/xrgame/linux-steam/build-rootfs.sh) L5、L10–17 |
| PRoot 绑定整个 `/dev`，进程能看到 `/dev/kgsl-3d0` 和 `/dev/dma_heap`；在 targetSdk 36 下能否打开尚未验证 | [LinuxSteamRuntime.kt](../../app/src/picoXr/java/app/gamenative/xrgame/steam/LinuxSteamRuntime.kt) L121–139 |
| `requestObserver` 常开，每个请求都分配一个 List 和一个 `JSONArray`，并进入同步块 | LinuxSteamService.kt L47–49、L140–142 |

15 Hz 的待验证假设，按可能性排序：CEF CPU 光栅（帧间隔约为 4 个 vsync，像是上游产帧受限）；无 MIT-SHM 时，Xephyr 对每个损坏区域单独 PutImage 并同步一次；X 线程与 GL 线程争用 `DRAWABLE_MANAGER`；1280×720 BGRA 每帧整帧上传。**这些都没有测过，不能据此预估收益。**

## 3. DroidDeck 的 Big Picture 硬件链（源码事实）

```text
[glibc / PRoot]
steamwebhelper: -cef-force-gpu -cef-use-gl=angle -cef-use-angle=vulkan -cef-ozone-platform=x11
  → Zink / Vulkan → glibc Turnip（KGSL）→ /dev/kgsl-3d0
  │ X11（gamescope 的 Xwayland :0；推断使用 glamor/DRI3，空闲时有 39 个 dma-buf）
gamescope 3.16.29-p9（--backend wayland，Vulkan on Turnip）
  │ Wayland：zwp_linux_dmabuf_v1 wl_buffer（LINEAR 或 UBWC），SCM_RIGHTS 传 fd
[bionic / app 进程]
waylandcomp：dma-buf → VkImage（adrenotools Turnip，每个 wl_buffer 只导入一次）
  Choreographer tick，只在有变化时绘制 → vkCmdBlitImage → SurfaceView 的 VkSwapchain
  （zero-copy 开关只在 HDR 时打开：blit 到 AHB 池 → ASurfaceControl）
```

| 机制 | 固定源码 | 对我们的意义 |
|---|---|---|
| 大屏 CEF 参数；只有 KDE 桌面因 QPainter/shm 才改用 `-cef-disable-gpu` | [droiddeck-session](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-session) L883–888 | 参数本身可直接借用，但前提是 GPU/WSI 已打通 |
| `MESA_LOADER_DRIVER_OVERRIDE=zink`、`GALLIUM_DRIVER=zink`、`LIBGL_KOPPER_DRI2=true`、`VK_ICD_FILENAMES`、`TU_DEBUG` | [SessionService.kt](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/session/SessionService.kt) 约 L610–627 | VGUI 的 GLX 可由 Zink 在 Turnip 上提供 |
| 伪造空的 `/proc/bus/pci/devices`：Chromium GPU 进程的 libpci 会 `exit(1)`，CEF 随后回退到 SwiftShader | [LinuxRuntime.java](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/runtime/LinuxRuntime.java) L224–236 | **必须借鉴**，否则“打开了 GPU”实际仍是 CPU 绘制 |
| 把 `/dev/kgsl-3d0` 伪装成 render node，并配 `drm.c` 的 GEM/PRIME 句柄表 shim | LinuxRuntime.java L291–340；`tools/linuxfs/preload/drm.c` | gamescope 的 linux-dmabuf 依赖它；R-B 先尝试不用（§5.2） |
| `kgslpoll.c`（避免零超时等待阻塞）、`kgslid.c`（A840v2 chip id 重映射） | `tools/linuxfs/preload/` | glibc Turnip 上机时可能遇到同类问题，先以我们 Turnip fork 的现有支持为准 |
| compositor 的默认分支：导入 dma-buf 后一次 GPU blit 到 FIFO swapchain，不取客户端 acquire fence，依赖 gamescope 先完成 | `app/src/main/cpp/waylandcomp/src/vk_present.c` L954–1031、L1615–1646 | 与我们“生产者先 CPU 等 fence 再 Present”的做法相当 |
| Quest 只有 2D 面板，没有 OpenXR | [quest.md](../../references/DroidDeck/docs/quest.md) L3–4 | XR 部分没有可抄的现成实现 |

只依赖 targetSdk 28/PRoot 的部分：从 app 数据目录执行文件、`-i uid:uid`、GPU ioctl 不经 tracer。我们的客户端已在 targetSdk 36 的独立 PRoot 中运行；但 glibc 进程打开 KGSL、dma-heap 能否成功，必须在 H0 实测。上游性能数字（例如 libpci 修复后 44 → 85 fps）只作为上游声明，不换算成我们的预期。

## 4. 差距

| 方面 | DroidDeck | 当前 SteamPSP 客户端 | 处理 |
|---|---|---|---|
| guest GPU 驱动 | Arch 预编译 Mesa（Turnip KGSL + Zink） | 无，只有 llvmpipe | 用 `tencentmalos/mesa-mirror` 固定源码构建 glibc Mesa，不用任何预编译驱动 zip |
| CEF | GPU（ANGLE/Vulkan 或 Zink） | `-cef-disable-gpu` | H1 切换；默认保留软件回退 |
| VGUI GLX | Zink（kopper） | Xephyr + llvmpipe | §5.2 的 B-1/B-2 二选一 |
| 显示服务器 | gamescope + Xwayland + app 内 Wayland compositor | Xephyr → Java X server | 保留 Java X server，补 dma-buf 版 DRI3/Present |
| 宿主导入 | adrenotools Turnip 导入 dma-buf | GL 纹理整帧上传 | VulkanRenderer（已支持 adrenotools）增加 dma-buf 导入 |
| 节奏 | Choreographer + dirty 合并 | `WHEN_DIRTY`，无节流 | 沿用 VulkanRenderer 的现有帧节奏 |
| XR | 无 | 游戏已有 Swan 2D 大屏（XR） | 面板来源抽象（§6） |

## 5. GPU 路线

### 5.1 候选比较

| 路线 | 内容 | 判断 |
|---|---|---|
| R-A 复刻 DroidDeck | gamescope + Wayland compositor + 伪 DRM | **暂缓。** 要多维护两层显示服务器，与现有 X server 的输入、IME、焦点、XR 帧桥重复，工作量最大。只在 R-B 的 H0 全部失败时重新评估 |
| **R-B 直连 X server（推荐）** | glibc Turnip/Zink → DRI3 dma-buf + Present → Java X server → VulkanRenderer | 复用已验证的游戏侧 Present/lease/异步 copy 和 XR 帧桥；新增部分集中在 glibc Mesa 构建、一个 WSI 分支和 DRI3 的一个 modifier 分支 |
| R-C Xephyr glamor | 让 Xephyr 在 GPU 上合成 | 需先核对 Xephyr glamor 依赖宿主的哪种 GL 接口（GLX 还是 EGL）。如果需要宿主 GLX 或 DRI3，宿主侧要做的事与 R-B 一样，还多一层合成，因此不选 |
| R-D 只优化软件链 | MIT-SHM、按区域上传、减少锁、换 Vulkan 渲染器 | 作为 S1 和长期回退保留；CEF CPU 光栅的上限不变 |

### 5.2 R-B 的两个子决策

**VGUI 的 GLX（H1 原型时决定）：**

- **B-1（先试）**：Xephyr 只承载 bootstrap/更新器窗口（它依赖核心字体，例如尚未实现的 `PolyText16`）和 steam 主进程的 VGUI GLX。只有 steamwebhelper 改用 `DISPLAY=:0` 并使用 GPU，由现有客户端 preload（`client-proc.c` 的 exec 钩子）在 exec 时改写环境，不修改 Valve 文件。待验证：steam 与 steamwebhelper 之间是否交换 X window ID；CEF 窗口映射到 :0 后焦点是否正确；非 ASCII IME 目前经 :1 的 CLIPBOARD 提交，换到 :0 时 Java X server 的 selection 支持是否够用。
- **B-2（B-1 不可行时）**：在 Java X server 实现“只支持直接渲染”的最小 GLX（QueryVersion/QueryServerString/GetVisualConfigs/GetFBConfigs/CreateContext 的 direct 形式/CreateWindow/IsDirect 等，不做间接 GL 协议），VGUI 经 Zink/kopper 走与 CEF 相同的 WSI。若还要去掉 Xephyr，需另补核心字体绘制。工作量明显大于 B-1。

**buffer 方向与宿主导入（H0 决定）：**

- **I-1（首选）**：客户端从 dma-heap 分配 LINEAR RGBA，导出 dma-buf，经 `DRI3 PixmapFromBuffers` 送达宿主；宿主 VulkanRenderer 用 `VK_EXT_external_memory_dma_buf` 导入后 copy 或采样。依据：VulkanRenderer 已能经 adrenotools 加载 Turnip（[VulkanRendererContext.cpp](../../app/src/main/cpp/winlator/VulkanRendererContext.cpp) L199–203），bionic Turnip 在 Windows VR unixlib 中已导出 dma-buf（[gamenative_openxr_unix.c](../../app/src/main/windows/openxr_runtime/unix/gamenative_openxr_unix.c) L672–802）。待验证：客户端页面实际用的是哪个驱动（系统 Adreno 驱动未必支持 dma-buf/modifier 导入），以及 glibc 构建在 KGSL 上能否导出。
- **I-2**：宿主走 `EGL_EXT_image_dma_buf_import`。导入代码已在 [xr_windows_projection.cpp](../../app/src/main/cpp/xrimmersive/xr_windows_projection.cpp) L160–201，同文件还有 CPU mmap 回退（L255–315）；但 SBS 最终验收走的是 guest 侧 dma-buf → AHB 中继，**厂商 EGL 是否支持未证实**。
- **I-3（最后备选）**：宿主分配 AHB，取 native handle 中的 dma-buf 交给客户端导入。这依赖非 NDK 的 `AHardwareBuffer_getNativeHandle` 和 gralloc 布局，跨设备风险最高。

### 5.3 呈现协议与同步

- glibc Turnip 新增一个 `XRGAME_X11_DMABUF` WSI 分支，对应现有只在 `__ANDROID__` 下编译的 [turnip-x11-ahb.patch](../../tools/xrgame/patches/turnip-x11-ahb.patch)（L47–344，modifier `1255`）。内容：分配可导出的 LINEAR 图像；`PixmapFromBuffers` 携带真实 dma-buf fd 和 stride，用一个不与 `1255`/`1274` 冲突的私有 modifier 标识；跳过 xshmfence；Present 前由生产者 CPU 等自己的渲染 fence。DRI3Open 仍不返回 fd，沿用游戏侧做法，避免依赖伪造的 render node。
- 只给客户端 X server 恢复 DRI3/Present，只新增 dma-buf 分支；SYNC 继续移除。`DRI3Extension` 增加 modifier 分支，生成新的 dma-buf 图像对象；`PresentExtension` 的 GPU 分支接受它；Idle 沿用 `HardwareBufferLeases` 语义，在宿主 copy 退休后发送。
- KGSL 的 dma-heap buffer 不带隐式 fence，第一版必须由生产者在 Present 前等待。以后若要去掉这次等待，另设计 sync_file 传递（DRI3 FenceFromFD 当前未支持），不能直接删除等待。
- 任一环节初始化失败（ICD、KGSL、dma-buf 导入、Chromium GPU 进程）都必须回退到当前软件链，并输出回退原因；Chromium 自动落到 SwiftShader 也算回退，不能计为 GPU 成功。

## 6. OpenXR 大屏

XR 合成器的合同已经与生产者无关：5 个 RGBA8 AHB 的 ring，用 held 位和 serial 同步，不需要 sync fd；生产者较慢时重复显示当前帧（[xr_frame_exchange.h](../../app/src/main/cpp/winlator/xr_frame_exchange.h)；VulkanRendererContext.cpp L1034–1040；`xrimmersive/xr_vulkan_compositor.cpp`）。阻碍 Big Picture 上头显的是外围对游戏的耦合：

| 耦合点 | 位置 |
|---|---|
| 入口必须带 `appId`，否则 `finish()`；Activity 内会启动 `XServerScreen` 和 Wine 容器 | [ImmersiveXrActivity.kt](../../app/src/main/java/app/gamenative/ui/screen/xr/ImmersiveXrActivity.kt) L343–348、约 L373 |
| 生产者只能通过全局 `PluviaApp.xServerView` 找到；客户端的 X server 是另一个独立实例 | 同上，约 L1473 |
| 手柄、指针、键盘轮询都要求 `WinHandler` | 同上，L866–872 |
| 尺寸、刷新率和合成环境变量取自 `Container`；退出时 `exitSteamApp` 并调用 `shutdownEnvironment()` | 同上，约 L510–537、L699–700 |
| 客户端页面是 GLRenderer；GL 桥只有单个 AHB 且没有同步 | LinuxSteamActivity.kt L101；`DirectGLBridge.kt` |
| 每个会话只有一个场景、一个 quad | `xr_immersive.h`；`xr_immersive.cpp` 约 L676–692 |

阶段：

- **X0**：客户端页面改用 VulkanRenderer（与 S1 共用），具备 XR 场景 ring 输出能力。
- **X1**：抽出“XR 面板来源”接口，至少包括场景 ring、输入接收端、生命周期所有者和设置来源，现有 Wine 游戏实现与新的官方 Steam 实现并列。Steam 实现的输入：手柄按钮映射为 Big Picture 导航（与 `LinuxSteamActivity` 的 A/B 映射一致）；射线的面板 UV 转成客户端 X server 的 `injectPointer*`；`xr_keyboard` 文本交给 `SteamImeSession`；HUD 帧率取客户端渲染器。退出 XR 不得关闭官方 Steam 会话。
- **X2**：Swan 实测：会话建立、`directRender=true`、72/90 Hz、输入与 IME、退出后重进、PRoot/Steam 进程在 Activity 切换时保持。
- **X3**：同一个 XR 会话内，从 Big Picture 启动游戏时把 quad 的 ring 切到游戏，退出后切回。这依赖官方 Play 生命周期先通过验收（目前在共享库记录中仍待验证）。
- **X4（可选）**：游戏中叠加 Steam 面板，需要多来源合成；另做 `XR_KHR_android_surface_swapchain` 探针，Pico 是否支持未知。

## 7. 阶段与门槛

| 阶段 | 内容 | 门槛 / 停止条件 |
|---|---|---|
| **G0 测量** | ① Steam `-cef-enable-debugging -devtools-port`（仅 debug 包、仅 PRoot 内回环）经 CDP 取 CEF 自身产帧与 GPU 信息；② `requestObserver` 改为 trace 开启时才分配，并加单调时间戳和 PutImage 字节数；③ 宿主统计渲染帧数和上传耗时；④ 逐线程 CPU（steamwebhelper 渲染/光栅、Xephyr、XConnectorEpoll、GLThread）；⑤ 固定场景：Big Picture 首页由注入按键匀速滚动 N 秒，A/B/A | 输出瓶颈归因表。CEF 自身产帧约等于显示帧 → 光栅受限，进入 H；CEF 产帧明显高于显示帧 → 传输/上传受限，先做 S1。没有归因就不承诺收益 |
| **S1 软件链** | 页面换 VulkanRenderer；按损坏区域上传，缩短持锁；探测 Xephyr ↔ 宿主 MIT-SHM 能否与客户端 `sysv.c` shim 互通；trace 默认关闭 | 同场景 A/B/A 的呈现 Hz 与 CPU；颜色（`needsSwapRB`）、焦点、IME、光标无回归；不影响游戏 X server |
| **H0 可行性探针** | ① 从固定源码构建 glibc Mesa（`-Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers=zink -Dplatforms=x11`）+ Ubuntu `libvulkan1`；在 targetSdk 36 的 PRoot 中运行 vulkaninfo 和离屏渲染，记录 chip id；② 导出 LINEAR dma-buf；③ 宿主按 I-1/I-2 导入并逐像素校验；④ 伪造 PCI 列表后，确认 Chromium GPU 进程没有落到 SwiftShader，并核对设备上 steamwebhelper 自带的 ANGLE/libvulkan；⑤ 确认 WSI/Zink 是否离不开 render node | KGSL 在 glibc PRoot 中打不开 → 停止 H，转 S1 并评估 R-A。I-1/I-2 都导入失败 → 评估 I-3，否则停止。只有探针包或 debug 开关，不进默认路径 |
| **H1 原型** | §5.3 的 WSI 分支与 DRI3/Present dma-buf 分支；CEF GPU 参数；按 B-1 / B-2 处理 VGUI；`XRGAME_STEAM_GPU=1` 默认关闭 | 正确性：颜色、尺寸变化、IME 叠加、焦点、光标；30 分钟稳定；fd/RSS 不增长；任一失败带原因回退软件链；与 S1 同场景比较 |
| **H2 产品化** | Mesa glibc 配方、rootfs component manifest、`THIRD_PARTY_NOTICES`、APK 审计（不得出现 `libvulkan_wrapper`/`libkgslshim` 等禁用名）；DebugBus 输出 `path=software\|gpu`、回退原因、呈现 Hz | AYN 验收记录满足 AGENTS 证据要求；默认开启前再由用户确认 |
| **X0–X4** | §6 | 各阶段单独留证；Swan 上机须用户另行确认 |

依赖关系：S1 的“改用 VulkanRenderer”同时是 H1 和 X0 的前置；H0 的 Mesa 构建只需要构建机，可以与 G0 并行；X1 的接口重构不依赖 H 轨。

## 8. 风险与未知

| 风险 | 影响 | 处理 |
|---|---|---|
| targetSdk 36 下 glibc 进程打开 KGSL/dma-heap 受 SELinux 限制 | H 轨失效 | H0 第一项就测；失败即停 |
| Turnip glibc 构建上游未覆盖 KGSL + X11 组合 | 构建或运行失败 | 补丁只放在我们的 fork 分支，遵守 Mesa 的 AI 贡献限制，不向上游提交 |
| 宿主驱动不支持 dma-buf 导入 | I-1 失效 | 让客户端页面强制使用 adrenotools Turnip，或改用 I-2/I-3 |
| CEF 实际使用 ANGLE-Vulkan 还是 Zink GL 不确定（DroidDeck 注释与参数不一致） | 调参方向错误 | H0 ④ 用 CDP GPU 信息确认 |
| steam 与 steamwebhelper 跨 display 失败 | B-1 不可行 | 改做 B-2 |
| 加入 GPU 驱动后 rootfs 体积与更新流程变化 | 打包和审计 | H2 重新计量，沿用 manifest/SHA 校验 |
| 与游戏争用 GPU/内存（Steam 在后台时） | 游戏帧率下降 | 游戏前台时客户端不出帧或降频；记录 GPU 占用 |

## 9. 证据要求

每次实测按 AGENTS 规则记录：APK SHA-256、新增 `.so` 和 glibc Mesa 的 Build ID、客户端 component manifest/rootfs SHA、设备型号/构建/boot id、App、PRoot、Xephyr、Steam、CEF 各进程 PID 及时长。失败和中止的采样也保留。FPS 数字只报本次测得的值，并注明场景和测量层（CEF 产帧、X server 帧、SurfaceFlinger 呈现或 XR 帧）。设备连接、刷写和安装前先征得用户同意。

## 10. 建议的第一批工作

1. G0：trace 改为按需分配并加时间戳；接入 CDP 产帧采样；定义固定滚动场景。需要 AYN 上机。
2. S1/X0 共用前置：客户端页面改用 VulkanRenderer，核对其 CPU drawable 上传与 XR 场景 ring 输出。
3. H0 ①：在构建机上写 glibc Mesa 配方，产出探针用 ICD；上机部分等 G0 完成后一起做。

## 11. 实施记录（2026-10-17，H1 原型与 Thor 实测）

### 11.1 两个子决策的实际选择

- **B-0（新增，替代 B-1/B-2 作为首选）**：CEF 继续连接 Xephyr `:1`，窗口管理、焦点、IME、字体和 VGUI 的 GLX 都不变。只有 Vulkan 帧绕开 Xephyr：glibc Turnip 的 WSI 在 `XRGAME_X11_HOST_DISPLAY=:0` 时，为每个 surface 在 Android X server 上建一个 override-redirect 窗口，跟随 surface 窗口的根坐标、尺寸和可见性，再按游戏的 DRI3/Present 链呈现。B-1 要求 Chromium 直接连 Java X server，需补 XI2/XKB/RandR/XFixes 等协议，v3–v11 的经验表明代价不可控，因此放弃。
- **I-3'（宿主分配 AHB，替代 I-1/I-2 作为首选）**：客户端用 DRI3 `PixmapFromBuffers` 发 modifier `1256`，`offset0` 放 AHB 格式，并附一个 socketpair。宿主以 `GPU_SAMPLED|GPU_FRAMEBUFFER|CPU_READ_RARELY` 分配 AHB（CPU 用途使 gralloc 不做 UBWC 压缩），用 `AHardwareBuffer_sendHandleToUnixSocket` 回传。客户端解析扁平化的 GraphicBuffer（`GB01`），取首个 fd（dma-buf）和 stride，以显式 LINEAR 布局导入。宿主侧因此与游戏 `1255` 路径完全相同（`GPUImage` → VulkanRenderer → lease/async copy → XR ring），不依赖系统驱动能否导入 dma-buf（目前没有证据）。代价是依赖 `GB01` 这一 Android 内部序列化格式：客户端会校验 magic、尺寸、stride 和 fd 数，不符即失败；Android 13（AYN）和 16（Swan）都需上机核对。
- 同步沿用 `XRGAME_X11_AHB`：Present 前由客户端 CPU 等待自身 fence，宿主发 IdleNotify 后客户端复用图像；不使用 SYNC fence 和 xshmfence。
- 第一版不需要 Zink：VGUI 仍在 Xephyr 内用 llvmpipe，CEF 参数为 `-cef-force-gpu -cef-use-gl=angle -cef-use-angle=vulkan`。**Thor 实测推翻了这一条和 B-0 的前提**，见 §11.4。

### 11.2 改动

| 部分 | 内容 |
|---|---|
| Mesa | [turnip-x11-host-ahb.patch](../../tools/xrgame/patches/turnip-x11-host-ahb.patch)：新增 `wsi_common_x11_host.inc`；只启用 KGSL 时 `HAVE_X11_DRM` 为假，按 Android 补丁的方式启用 Present 路径；宿主模式下跳过 surface server 的 DRI3 要求和 Present 能力查询（Xephyr 没有 Present 扩展，查询会断开连接），回收时不调用 SYNC/XFIXES |
| 构建 | [build-gpu.sh](../../tools/xrgame/linux-steam/build-gpu.sh)：与 Android Turnip 用同一 pin（2026-10-19 起为 `55186f1f`）和前 5 个补丁，再加宿主补丁和 [kopper 读回补丁](../../tools/xrgame/patches/mesa-kopper-pixmap-readback.patch)；同时构建 Zink（`libgallium`）和 glvnd GLX，厂商名 `steampsp`（`libGLX_steampsp.so.0`），不影响 Ubuntu 自带的 Mesa。在 Ubuntu 24.04 的 arm64 环境原生构建，或在 x86_64 上用 multiarch 交叉构建。另从签名的 Ubuntu 仓库取 libvulkan1 和 X11/XCB 运行库 deb，记录版本和 SHA；`gpu-build.json` 列出所有文件的 SHA 和 Build ID。`stage.py --gpu` 打包为可复现的 `gpu.tgz`，篡改即拒绝 |
| 宿主 | `DRI3Extension` 新增 `1256`；`GPUImage`/`gpu_image.c` 负责宿主分配并发送；`Window.isInputPassthrough()`：带 `_XRGAME_INPUT_PASSTHROUGH` 的窗口不参与指针命中测试。修复两处既有泄漏：DRI3 的三种 pixmap 都未登记为客户端资源，断开后不释放，而资源 ID 段会被下一个客户端复用，导致 `BadIdChoice`；Present 的事件选择在客户端断开或窗口销毁后残留，复用同一事件 ID 时报 `BadMatch`。CEF GPU 进程每次崩溃重启都会触发这两处。另外，带 `_XRGAME_STACK_KEY` 的窗口始终位于根窗口其他子窗口之上，并按键排序（`Window.restackKeyedChildren`），使各覆盖窗口保持 Xephyr 中的层级 |
| 客户端 | `LinuxSteamGpu`（2026-10-18 起默认开）；GPU 模式下保留客户端 X server 的 DRI3/Present（SYNC 仍移除），页面改用 VulkanRenderer，override-redirect 和透传窗口不抢焦点，PRoot 在 `/proc/bus/pci/devices` 不可读时挂载空文件，组件安装到 `/opt/steampsp-gpu`。`launch-client.py` 只给 Steam 设置 ICD、`LD_LIBRARY_PATH`、宿主显示和 `XRGAME_WEBHELPER_GL_VENDOR`，Xephyr 不受影响；客户端 shim 据此在 webhelper 浏览器主进程里切到 `steampsp` 厂商的 Zink，Chromium 子进程继承（§12 L1）；GPU 模式下 CEF 参数为 `-cef-force-gpu` |
| 调试 | DebugBus `linux_steam gpu on|off`，状态返回 `gpuRequested/gpuBundled/gpuDisplay` |

### 11.3 已验证与未验证

- 已验证（构建机）：WSL Ubuntu 24.04 x86_64 交叉构建通过，补丁编译无新增警告。库导出 `vk_icdGetInstanceProcAddr`，依赖里有 `libxcb-present`，不依赖 libdrm/xshmfence。`build-gpu.sh` 从固定源码端到端跑通；组件约 17.5 MB（未压缩）。Python 测试 28 项通过，覆盖 GPU 环境只作用于 Steam、组件缺失时回退，以及 `gpu.tgz` 的可复现性和篡改拒绝。WSL 中定向运行 `testPicoXrDebugUnitTest` 40 项全部通过：新增透传 1 项（Robolectric）和 GPU 模式 3 项，其余为既有的 X11 wire 测试和官方 Steam 测试。picoXr 主源码随之编译通过。本机缺少官方 Steam staging、新 catalog 的 runtime bundle 和 `references/Vulkan-Headers`，因此运行时排除了 `stageLinuxSteam`、`filterXrGameAssets` 和原生构建三个任务。
- 测试包：本机 WSL 自建全部输入后打出 v1（`a6dc4598…`，Odin 3）、v2（`172b4d21…`）和 v3（`1aa5492f…`，Thor），审计均为 149/0。见[验证记录](../validation/big-screen-gpu-20261017.md)。
- Thor 已验证：glibc Turnip 在 targetSdk 36 的 PRoot 中能打开 KGSL；`GB01` 解析与 LINEAR 导入、覆盖窗口的位置与颜色由独立 Vulkan 探针验证（约 60 fps）。CEF 走 GPU 失败，原因见 §11.4；帧率收益无从测量。
- 页面在后台时：`detachRenderer()` 会清空渲染器，GPU 帧落入 Present 的 CPU `copyArea` 分支；宿主分配的 GPUImage 没有 CPU 数据，`copyArea` 直接返回，该帧被丢弃，Complete/Idle 照常发出，客户端不会卡住。回到前台后，下一帧恢复显示。
- 已知限制：Xephyr 的软件光标被 GPU 覆盖窗口遮住；Xephyr 内叠在 CEF 窗口上方的 VGUI 对话框也会被遮住；覆盖窗口的原点假设 Xephyr 的宿主窗口位于 (0,0)。

### 11.4 Thor 实测结论（2026-10-17 白天；当晚在 Odin 3 上修正，见 §12.1）

证据见[验证记录](../validation/big-screen-gpu-20261017.md)的“Thor 实测”。

- **（已被 §12.1 修正）当时的判断是 B-0 不适用于 Steam 大屏。** 依据是 Valve 合成器用 `GLX_EXT_texture_from_pixmap` 读 Chromium 的 X 窗口。Odin 3 上的逐线程测量表明，合成器平时几乎不工作，大屏主画面是 CEF 直接画进自己的 X 窗口；只要 CEF 的 GPU 进程和合成器都跑在 Zink 上，B-0 就成立。白天的失败有两个原因：一是 ANGLE-Vulkan 让合成器的 llvmpipe 触发 SIGILL，二是 Zink 的 kopper 在没有 DRI3 的 Xephyr 上发 DRI3 请求，导致连接被关闭。
- **Steam 不转发 `-cef-use-gl`/`-cef-use-angle`。** 用 shim 强加 ANGLE Vulkan 后，GPU 合成改变了 Valve 合成器的 GL 路径，Ubuntu llvmpipe 的 JIT 代码触发 SIGILL，webhelper 每 10 s 重启。这个注入已从源码撤回。
- **DroidDeck 的整条链都在 Zink 上，它的 X server（gamescope 的 Xwayland）也有 DRI3。** 当时在 Xephyr 上用 `LIBGL_KOPPER_DRI2=true` 让 webhelper 跑 Zink：合成器和 CEF 都能经宿主 WSI 建出 swapchain，但合成器在 `glXBindTexImageEXT` 时触发 Xlib `Fatal IO error 11` 而退出。Mesa 源码证实，原因是 kopper 对 pixmap 硬性发 DRI3 请求；`mesa-kopper-pixmap-readback.patch` 已修复。
- 保留的改动：X server 的 DRI3/Present 资源泄漏修复（与 GPU 模式无关，游戏同样受益）、宿主 AHB 呈现链（I-3'，探针已在 Thor 验证），以及默认关闭的 GPU 模式开关。

接下来的路线需要重新选择（当天按用户方向重排，以 §12 为准）：

| 路线 | 做法 | 主要代价与风险 |
|---|---|---|
| R1：Xwayland 替换 Xephyr | 与 DroidDeck 一致：Xwayland 提供 DRI3/glamor，Valve 合成器和 CEF 都走 Zink → Turnip；我们实现一个只服务 Xwayland 的最小 Wayland 合成器（linux-dmabuf → AHB/VkImage），或让 Xwayland 的根窗口经现有宿主 WSI 呈现 | glamor 需要 GBM/EGL 和 render node，KGSL 没有，DroidDeck 用 DRM shim 伪装；工作量最大，属于新的 B 阶段 |
| R2：优化现有软件链 | 不改架构：Xephyr 与 Java X server 之间恢复 MIT-SHM（PutImage 免拷贝）、按损坏区域上传纹理、X 线程与 GL 线程不再争 `DRAWABLE_MANAGER`，并测量 Valve 合成器读回的耗时 | 收益上限受 CPU 光栅与 llvmpipe 合成限制；需要先测量 15 Hz 的真正瓶颈（§2 的假设仍未测） |
| R3：Steam 直连 Java X server（B-1） | 补 GLX、Composite、texture-from-pixmap、XI2/XKB 等 | 等于在 Java X server 里实现完整的 GPU X 栈，§11.1 已判定不可控 |

OpenXR 大屏（§6）不受影响：XR 合成器只消费 `XrFrameExchange` 的 AHB ring，软件链的最终纹理同样可以送进去。

## 12. 分层重评（2026-10-17，按用户方向）

用户方向：官方 Linux Steam 看重的是“官方原生”属性，工作重心在游戏适配（OpenXR 等），成本要尽量小；如果能基于当前实现做出更简单的模式，就优先用当前实现，兼容性更有把握；目标硬件是 A8xx。

**A8xx 的事实。** 以下都是 DroidDeck 自己的记录，我们没有测过：

- 0.1.4（2026-09-22）的现场报告：一台 Fold 的客户端菜单只有 10–20 fps（Chromium → ANGLE → Zink，实验性 A8xx Turnip）。
- 0.1.5 的发布说明解释了原因：Chromium GPU 进程被 libpci 杀掉，CEF 退回 CPU 绘制；绑定一个空的 devices 文件后修复。修复后 Adreno 830（A8xx）Fold 上大屏 117–118 fps。
- 0.3.0 记录 Adreno 840v2 用运行时自带的驱动可以工作。

我们的运行时已经做了同样的 libpci 规避（`LinuxSteamRuntime`）。Thor（A7xx）上回退到软件，原因在架构（Xephyr 没有 DRI3，见 §11.4），与 A8xx 无关。

**关键约束**（写于实施前；§12.1 的测量表明合成器平时几乎不工作，这一条只影响它读 pixmap 的少数情况）。任何 GPU 模式都要解决 Valve 合成器的 texture-from-pixmap。Mesa 源码证实：Zink 的 kopper 对 pixmap 只要不是软件屏，就调用 `loader_dri3_get_pixmap_buffer`（`src/gallium/frontends/dri/kopper.c` 约 L324），即 DRI3 `BuffersFromPixmap`。Xephyr 没有 DRI3，xcb 发出不支持的扩展请求会直接关闭连接，这就是 Thor 上的 `Fatal IO error 11`。同一文件的 `kopper_update_tex_buffer`（约 L449）已经有 drisw 的读回路径（`XGetImage`/`XShmGetImage`），只是被 `has_dmabuf` 挡住了。

| 层 | 内容 | 省掉什么 / 代价 |
|---|---|---|
| L0：保底 | 软件路径保持默认和回退；X server 的 DRI3/Present 泄漏修复进主干 | 无新增组件 |
| L1：webhelper 上 GPU（基于当前实现，**已实施，见 §12.1**） | 最初只计划把 Steam 合成器换到 Zink（v4），测量证明无效。实施版本是 v5：webhelper 的浏览器主进程改用组件自带的 glvnd 厂商 `steampsp`（Zink → glibc Turnip，`LIBGL_KOPPER_DRI2=true`，去掉 Steam 设的 `LIBGL_KOPPER_DISABLE`），Chromium 子进程继承这些设置；CEF 用 `-cef-force-gpu`。CEF 的 GPU 进程（ANGLE GL 后端 → GLX → Zink）和合成器的每个窗口都经宿主 AHB 链在 :0 上有自己的覆盖窗口；WSI 写入 `_XRGAME_STACK_KEY`，Java X server 按它维持 Xephyr 的层级。kopper 读回补丁保证合成器读 Xephyr pixmap 时不再断开连接 | 省掉 CEF 的软件合成与光栅、Xephyr 的转发，以及 Java 侧的拷贝和整帧上传。代价：Xephyr 的软件指针被覆盖窗口挡住；宿主补丁多了叠放键 |
| L2：软件链优化 | Xephyr 与 Java X server 之间恢复 MIT-SHM、按损坏区域上传、X 线程与 GL 线程不再争 `DRAWABLE_MANAGER` | 与 L1 可以叠加：L1 之后 CEF → Xephyr 这一段仍是软件 |
| L3：跟随 DroidDeck | gamescope + Xwayland + KGSL 伪装的 render node + `waylandcomp` | 唯一能让 CEF 光栅也上 GPU 的路线；只在 L1、L2 的收益不够时考虑 |
| L4：客户端进 XR | 输出接入 `XrFrameExchange` | L1 的合成器帧本来就是 AHB，接 XR 最直接 |

**先测再选**（已完成，见 §12.1）：在 A8xx 设备上的软件模式下，滚动大屏时按进程和线程分开测 CPU，同时用 SurfaceFlinger 测呈现率。

游戏路径不受影响：官方 Steam 只负责登录、下载、云存档和启动，游戏仍交给 bionic Wine 容器，沿用现有的 X server、DRI3/Present、Turnip 和 OpenXR 链。

L1 的验收：A8xx 设备上，webhelper 跑在 Zink 上，没有 Xlib IO 错误；与软件基线在同一场景对比呈现率和各进程 CPU；弹出层的层级与软件模式一致；退出、重启和游戏 handoff 不回退。前三项已在 Odin 3 通过，最后一项未测。

### 12.1 Odin 3 实施结果（2026-10-17 晚）

证据见[验证记录](../validation/big-screen-gpu-20261017.md)的“Odin 3 实测”。

- **测量推翻了白天的假设。** 软件模式下，Steam 合成器所在的 webhelper 主进程只占 0.6% CPU，热点是 CEF 的 `VizCompositorThread`（约 0.9–1 核）和光栅；Xephyr 约 0.37 核。所以只换合成器的 v4 没有任何收益（登录页 30.84 对 29.71 Hz，CPU 2.17 对 2.15 核）。
- **v5 让 CEF 也走 GPU。** 同一 MHW 详情页滚动时，呈现率 26.36 → 41.40 Hz，帧间隔中位数 33.34 → 16.67 ms，P95 50.01 → 33.34 ms；总 CPU 1.71 → 0.58 核，其中 CEF 软件合成从 0.93 核降到 0.04 核，Xephyr 从 0.37 核降到 0.01 核。静止时两种模式都不出帧。
- **弹出层的层级。** 每个 Chromium 窗口各有一个覆盖窗口，必须保持它们在 Xephyr 中的上下关系，否则菜单会被主视图压住。叠放键按“根到窗口的兄弟序号”在 Xephyr 上计算：创建时计算一次，之后几何或可见性变化时、以及每 30 次 Present 刷新一次。
- **与 DroidDeck 的关系。** 做法与 DroidDeck 的“Chromium → ANGLE → Zink → Turnip”一致，但不需要 gamescope、Xwayland、render node 伪装和 Wayland 合成器。呈现走的是游戏已在用的 X server、DRI3/Present 和宿主 AHB 链。
- **未完成。** 鼠标指针在 GPU 模式下不可见；启动游戏、电源菜单退出、长时间运行、A7xx 上的 v5、Swan/XR 都未测。GPU 模式仍默认关闭。


## 13. Steam XR 模式（2026-10-20，按用户要求）

用户要求：进入 Steam 前在 Swan 上选择 2D 或 XR；Steam 客户端本身也能跑在 XR 大屏里；进 Steam 后不再为每个游戏保留 2D/SBS/XR 选项，普通游戏跟随入口；Alyx 这类 VR 专有游戏要能正确启动；SBS 暂时不需要；注意功耗。

### 13.1 决定

- **入口**：首页（LaunchActivity）顶部加「2D / XR」选择，只在头显上显示，存进官方模式的偏好（`XrGameOfficialSteam`）。它只决定官方 Steam；「其他」入口（Custom 库）保留原来的每游戏设置。
- **游戏显示方式**（只在官方模式）：
  - VR 专有游戏一律走 Windows VR（OpenXR 立体），与入口无关。判定依据是官方客户端 `appcache/appinfo.vdf` 中该应用的 `common/onlyvrsupport = 1`（Alyx 546560、The Lab 450390 为 1；Hades II、麻将没有这个键），容器里已标为 VR 的游戏也算。
  - 普通游戏：XR 入口走 2D 大屏（XR），2D 入口走 2D。
  - 启动面板隐藏「显示模式」「游戏类型」两行，改为显示最终方式；头显上不再选到 SBS。非头显设备上 VR 游戏仍按原规则回退 SBS。
- **Steam 客户端进 XR**：落实 §6 的 X1，但先不做 X3（同一会话切换来源）。
  - `ImmersiveXrActivity` 改为可继承，加「托管客户端」接口（画面来源、输入、键盘文本、生命周期）；picoXr 新增 `SteamXrActivity` 子类，托管客户端 X server 的渲染视图、`SteamImeSession` 和启动页。Wine 游戏路径不变。
  - 输入：射线是鼠标（手写笔语义，悬停移动光标），右摇杆滚轮，A/B 对应 Enter/Esc（与 `LinuxSteamActivity` 一致），双摇杆按下开关 XR 键盘，文本经 X 键盘送进客户端。
  - 从 Steam 启动游戏：`SteamXrActivity` 停止时释放自己的 XR 会话，游戏用自己的 `ImmersiveXrActivity` 会话；游戏退出后回到 `SteamXrActivity`，重新建会话。退出游戏后的返回目标按入口选择（2D 回 `LinuxSteamActivity`）。
- **功耗**：Steam 会话请求 72 Hz、不超分、不开注视点；游戏运行期间 Steam 会话不存在；叠加层（光标、菜单）只在光标移动或界面重绘时重画，静止时按空闲间隔检查。

### 13.2 XR 大屏对 Steam 还缺的（实施前清单）

| 项 | 现状 | 处理 |
|---|---|---|
| 画面来源 | 只能取 `PluviaApp.xServerView`（Wine） | 托管客户端提供渲染视图，GL/Vulkan 两种直通桥照用 |
| 尺寸/刷新率/合成设置 | 取自 Wine 容器 | Steam 用客户端屏幕尺寸、72 Hz、Vulkan 合成、无超分 |
| 位置/缩放/曲面/透视 | 存在游戏容器 | 单独存一份 Steam 的设置 |
| 指针 | 触摸手势（点按、拖动滚动、长按） | 手写笔直接映射鼠标，悬停移动，右摇杆滚动 |
| 手柄按钮 | 进 Wine 虚拟手柄 | 映射为 Big Picture 导航键 |
| XR 键盘 | 文本进 Wine X server | 文本进客户端 X server |
| 快捷菜单 | 由 `XServerScreen` 提供 | 长按 Start 打开 Steam 自己的小菜单：切到 2D、键盘、重置位置、透视、返回首页 |
| 游戏交接 | `LinuxSteamActivity` 在前台时处理 | `SteamXrActivity` 前台时同样处理；停止时释放会话 |
| 退出/清理 | 结束时 `shutdownEnvironment()`、`exitSteamApp` | Steam 模式不碰 Wine 环境，不关官方 Steam 会话 |
| 截取 | 已支持 `xr`/`canvas` | 照用，作为验证证据 |

### 13.3 阶段

- **S-1**：入口选择、游戏方式跟随入口、VR 专有游戏识别、启动面板隐藏两行。
- **S-2**：`SteamXrActivity` 显示客户端、输入、键盘、菜单、功耗措施。
- **S-3**：游戏交接与返回；Swan 验证 2D/XR 两个入口各一款普通游戏和 Alyx。

### 13.4 实施（2026-10-20，kb8，未上机验证）

- S-1：
  - `XrGameOfficialSteam` 增加 `display`（2D/XR）、`launchMode`、`applyDisplay`、`homeActivity` 和 `onlyVr`。
  - picoXr 新增 `SteamAppInfo`，读 `appinfo.vdf` v28/v29，按文件修改时间缓存。
  - `PluviaMain.launchHandedOverApp` 在显示面板前写入显示方式；启动面板加 `followsEntry` 参数，隐藏两行；退出游戏、取消面板时都按入口返回。
  - 首页右上角加「Steam 显示方式：2D 窗口 / XR 大屏」，只在头显上显示；切换时关掉另一种显示方式的 Steam 页面。
- S-2：
  - main 新增 `XrHostedClient` 接口，`ImmersiveXrActivity` 改为 `open`，托管模式下：
    - 画面来源换成托管客户端的渲染视图；
    - 设置存在 `steam-xr` 偏好；
    - 会话 72 Hz；
    - 用单独的轮询线程，不需要 `WinHandler`；
    - 不碰 Wine 环境；
    - 叠加层只在客户端页面重绘或光标移动至少 2 像素时重画。
  - picoXr 新增 `SteamXrActivity`、`SteamXrClient`，长按 Start 打开菜单：切到 2D、键盘、重置位置、调整大屏位置和大小、透视、返回首页、关闭。
- S-3：
  - `SteamXrActivity` 在 `onStop` 释放会话；回到前台时，如果进程里还有别的会话（游戏的，正在关闭），每 100 ms 检查一次，等它结束再建。
  - 游戏页面结束（`onPause` 且正在结束）时立即释放会话，不等 `onDestroy`。
- 首页面板（2026-10-20 用户反馈：进 XR 大屏后首页挡在前面）：Pico 把 `SteamXrActivity` 放进单独的任务，首页所在任务虽被标为不可见，它的 2D 面板仍留在前面。现在和游戏一样处理：`SteamXrActivity` 每次回到前台都把首页任务移到后面（`ImmersiveXrActivity.launcherActivity()`，游戏为 MainActivity，Steam XR 为 `LaunchActivity.current`），页面结束时再移回前面。菜单的「返回首页」「切到 2D」从首页自己的 Activity 启动目标页面，2D 的 Steam 页面落在首页任务里、在首页之上，首页移回前面时不会盖住它。「切到 2D」会把显示方式存成 2D，首页回到前台时重读显示方式（和尺寸、分辨率、透视一样），不再显示旧的选择。
- 单元测试：`XrGameOfficialDisplayTest` 3 项、`SteamAppInfoTest` 3 项，加上原有的 `XrGameLaunchModeTest` 7 项，全部通过。包 kb8 `7db8ae88…`，审计 0 errors。

### 13.5 大屏设置分工与游戏期间的 Steam 图层（2026-10-20，kb14，未上机验证）

用户先问 2D 游戏能否与 Steam 共用一个 OpenXR 会话，评估后决定不共用：

- **会话**：游戏用自己的会话和新图层呈现，与 Steam 的实现互不耦合。Pico 一次只允许一个会话，所以游戏会话起来前先结束 Steam 的会话（kb13）。
- **游戏期间的 Steam**：Steam 的图层隐藏，渲染器也停下。`SteamXrActivity.onStop` 关掉 Steam X server 的平面呈现：客户端的 Present 直接回 SKIP 和 idle，不拷贝也不合成，但记下每个窗口最后一帧。回到前台时重新打开呈现，并按记下的帧补画，Steam 空闲时也不会黑屏。
- **设置分工**（用户：Steam 开销小、游戏开销大，适用场景不同，但大屏尺寸可以共用）：
  - **共用**：大屏尺寸（65/80/100/120″ 预设或手动调出的自定义尺寸）、距离和弧形，存在 `steam-xr` 偏好（`XrGameOfficialSteam.xrScreen`），四处都能改：
    - 首页选 XR 时显示的选项行；
    - 游戏启动面板的尺寸行（从 Steam XR 进入时注明"与 Steam 大屏共用"）；
    - Steam 页面里拖动调整；
    - 游戏会话里调整。
  - 官方 XR 入口的普通游戏，在 `applyDisplay` 时把共用尺寸写进游戏容器。Steam 页面每次建会话前都重新读取共用尺寸。
  - **分开**：分辨率。Steam 自己的分辨率在首页选；游戏分辨率仍是每个游戏自己的，在启动面板里选。可选 1280×720、1600×900、1920×1080，默认 1920×1080（2026-10-20 用户要求；没存过或存的值不在三档里时也用它）（kb35 曾改为按单眼尺寸折算，用户决定回到固定三档，见下方「首页两排选项」）。客户端 X server 运行期间尺寸固定：在首页进 XR 时，如果正在运行的客户端尺寸与所选不同，就先停掉再重启。托管页面发现客户端屏幕尺寸变了（客户端重启后），会按新尺寸重建会话。
- Steam 菜单的「重置位置」只把大屏移回原位，不再把尺寸改回 80″。
- **首页两排选项**（2026-10-20 用户要求）：显示方式仍在标题行；选 XR 时下面两排，标签同宽、左对齐。首页固定显示英文（`LaunchActivity.attachBaseContext` 套英文 locale，其他页面不变）。
  - 每排左边是单选胶囊，右边缘是一个开关（文字加开关，整块是一个点按目标），和单选区分开、两个开关上下对齐。第一排：大屏尺寸（65/80/100/120″）和弧形。
  - 第二排：Steam 分辨率，固定 1280×720、1600×900、1920×1080（默认 1920×1080），和透视（VST）。透视是 Steam 大屏自己的（`XrGameOfficialSteam.xrPassthrough`，Steam 页面会话开始时读同一个设置）；游戏的透视仍在各自的启动面板里。kb35 曾按单眼尺寸和屏幕折算（80″ 为 1280×720、1392×782、1824×1026、2240×1260），用户看过后决定回到固定三档。
  - 说明文字一行的右边缘是状态层开关（2026-10-20 用户要求，默认可见），和上面两个开关排成一列。它就是游戏启动面板里的「状态层（所有游戏）」：同一个 `PrefManager.xrgameStatusLayer`，Steam 大屏和所有游戏的 XR 会话共用，会话里每次刷新叠加层时读取，所以在首页改了，回到 Steam XR 就生效。
- 单元测试：`SteamClientDisplayTest` 新增 XR 分辨率一项，连同 `SteamAppInfoTest`、`XrGameOfficialDisplayTest`、`XrGameLaunchModeTest` 共 16 项，全部通过。包 kb14 `be773606…`，审计 0 errors，未装机。

### 13.6 启动面板画在大屏上；XR 键盘的拼音输入（2026-10-20，kb15，未上机验证）

用户要求：

- 大屏模式下，游戏的启动面板直接用 ImGui 图层、按大屏大小画在大屏位置，不再切回 Android 的 2D Activity；
- XR 大屏支持输入法，参照 Foundation 的 ImGui 输入法（可以用独立图层）。

调查结论：Foundation 只有文本编辑模型（`TextEditSession`）和输入框对话框（`drawImeDialog`），没有拼音引擎、候选栏，也没有把系统输入法接进 ImGui；它自己的规格也写明，Pico 没有 OpenXR 文本输入扩展，系统输入法窗口合成不进 XR 会话。用户选择移植 AOSP 拼音引擎。

**启动面板**：

- Steam XR 页面收到 Steam 交来的游戏后不再启动 `OfficialSteamGameActivity`，由 `ImmersiveXrActivity.launchHostedGame` 交给 `XrLaunchPanel`（Kotlin）。它在 IO 线程执行 `applyDisplay` 并读取容器，把面板交给原生图层显示。
- 面板各行从 2D 面板里抽出来，成为共用的 `XrGameLaunchOptions`（行的标签、选项、选中项、说明、选择后的效果，以及保存）。2D 面板（Compose）和 XR 面板都只负责画这些行，逻辑只有一份。
- 原生 `xr_launch_panel.cpp`：
  - 自己的 ImGui 上下文，画布 1600×900；每帧取大屏 quad 的位姿和尺寸，提交顺序紧跟大屏，盖在它上面，状态层和键盘仍在最上面；
  - 射线加扳机点选，右摇杆滚动，A 开始，B 取消；
  - 模型由 Kotlin 以制表符分隔的文本传入，选择结果以事件行传回（`nativeSetLaunchPanel` / `nativeDrainLaunchPanel`）；
  - 截图借用 Foundation capture 闲置的 Environment 槽位（大屏会话没有环境场景），所以 SDK 的 `xr` 截图能截到面板。
  - 每行是自己的 ImGui ID 作用域（`PushID(section * 1000 + index)`）。2026-10-20 用户报射线指到选项时 ImGui 提示 ID 冲突：几行都有同样的选项（关/开），药丸按钮的 ID 只由序号和文字组成，不同行的同名选项 ID 相同；原来只有下拉框按行压了 ID。ImGui 只在指着的控件上检查重复，面板在发现时记一行 `ImGui ID conflict on the pointed item`（日志标签 `xrimmersive_launch`）。
- 点开始后保存选项，然后像库里启动一样调用 `preLaunchApp`：
  - 它的加载文字显示在面板上；
  - 它弹出的消息在面板上显示，确定后取消这次启动（Steam 不再等待）；
  - 只发了 Snackbar 的失败，在 1.5 s 内没有后续时显示通用失败提示；
  - 成功后由页面自己的 `MainViewModel.launchApp` 拉起游戏的沉浸式页面，Steam 页面停止时面板随之关闭。
- 面板打开期间，Steam 页面的轮询线程不把射线和 A/B 送给 Steam。

**拼音输入**：

- 引擎用 AOSP `platform/packages/inputmethods/PinyinIME` `49aebad1` 的 `jni/`（libgooglepinyin）和自带词库 `dict_pinyin.dat`（1 MB，Apache-2.0）。在 64 位上有两处要改：
  - 词库里的计数是 `uint32`，原代码直接读进 `size_t` 成员，高 4 字节保留原值；
  - `userdict.cpp` 包含平台才有的 `cutils/log.h`。
  - 两处修改是本地克隆上的两个提交，要先 fork 到 tencentmalos 才能作为子仓库进来（`app/src/main/cpp/extras/pinyinime`）。在那之前，构建脚本从仓库外复制源码。
- 词库打成资源 `assets/pinyin/dict_pinyin.dat`（APK 审计按 SHA-256 登记）。每次安装后由 `XrPinyinDictionary` 解包到 `files/pinyin/` 一次，用户词库也在那里，用来学习选过的词。
- 候选栏画在 XR 键盘面板的顶部，和键盘同一个图层（截图槽位每种只有一个）：
  - 第一行：中/EN 切换、组字框（组字时显示带下划线的拼音，否则显示已输入的文字）、关闭；
  - 第二行：候选，翻页按钮 ◀ ▶；
  - 字体改用系统的 Noto Sans CJK（mmap，ImGui 1.92 按需生成字形）。
- 中文模式下：
  - 小写字母进入组字；
  - 空格选第一个候选（整句）；
  - 回车把字母原样上屏；
  - 退格先撤销上一次选词，再删字母；
  - 逗号、句号、问号、叹号、冒号、分号换成全角标点。
- 设备语言为中文时，键盘默认中文模式。
- 送达 Steam：XR 键盘的文字、退格和回车都排进同一个 `SteamImeSession` 队列。ASCII 按按键输入；中文写进客户端剪贴板再按 Ctrl+V（与 2D 页面的输入法同一路径），先后顺序不会乱。
- **中文送进游戏**（2026-10-20，在本会话里接着做）：
  - X server 原来就有一条现成路径：按键前，如果键位上没有要发的 keysym，就先改映射、发 `MappingNotify`（Winlator 用它处理 Android 软键盘的非美式字符）。
  - winex11 收到 `MappingNotify` 会重新读键盘映射，给任何有 keysym 的键位分配一个备用虚拟键码；`X11DRV_ToUnicodeEx` 把 `0x01000100`–`0x0100FFFF` 的 keysym 直接转成 Unicode 字符（proton-wine `dlls/winex11.drv/keyboard.c`）。窗口于是收到 `WM_KEYDOWN` 和 `WM_CHAR`。
  - 因此 `IMEInputReceiver.typeCharacter` 遇到美式键位上没有的字符，改走 `typeUnicode`：keysym 为 `0x01000000 | 码点`（Latin-1 直接用码点），只支持基本平面。XR 键盘和 2D 输入法两条路都受益。
  - 键位用留出的 120–131 号（`XKeycode.KEY_UNICODE` 到 `Keyboard.LAST_UNICODE_KEYCODE`），轮流使用（`Keyboard.unicodeKeycode`）：
    - Wine 不是收到按键时定字符，而是窗口 `TranslateMessage` 时按**当时**的映射重查键位。kb22 只用 120 一个键位，「你好」两个字都被解成了「好」（见验证记录）。
    - 已映射到这个字的键位直接复用（不再改映射、不发 `MappingNotify`），否则取最久没用的键位。所以最多 12 个不同的新字可以同时等待翻译；一次拼音上屏远少于这个数。
    - winex11 按键位顺序给有 keysym 的键位分配备用虚拟键码，各不相同；键位一旦有了 keysym 就不再清空，分配结果稳定。
  - 上限 131 号是因为 X server 的 `GetKeyboardMapping` 回复每个键位只写一个字、索引按键位算，实际只覆盖到 131 号键位（这是原有的问题，没改）。
  - 走 `winhandler.exe` 不行：官方启动链里没有这个进程，它的 `KEYBOARD_EVENT` 也只带一个字节的虚拟键码。
  - 测试入口：DebugBus `guest_exec <程序>`。它借用游戏的 `explorer.exe` 进程：环境取自它的 `/proc/<pid>/environ`（同一个前缀、wineserver 和 esync 设置），去掉只属于那个进程的 `WINESERVERSOCKET`、`WINEPRELOADRESERVE`、`WINELOADERNOEXEC`（带着它们 wine 直接退出 1）；命令为 `wine explorer /desktop=<游戏的桌面> <程序>`，因为新进程默认落在另一个桌面，要另起 explorer 而失败（`nodrv_CreateWindow`）。前置的 `名=值` 覆盖环境（比如 `WINEDEBUG=key`，即 `+key`；用来核对 Wine 收到的字符：Wine 自带字体没有汉字，notepad 里看不见）；输出写到 `files/guest-exec.log`（管道没人读，话多的程序会卡住），退出码记在 logcat（`DebugBus`，I 级）。
- **界面样式**（用户要求参考 shadPS4，加图标、调样式）：shadPS4 没有图标字体，它合并 Noto Sans Symbols2 用 Unicode 符号当图标，用 ImDrawList 画手柄键位标签和按键形状；各面板自己设颜色。我们照这个做：
  - 字体：文字用 PICOSansSC，后备合并 Android 的 `NotoSansSymbols-Regular-Subsetted.ttf`（TrueType，覆盖 ⌫ ⏎ ⇧ ⇪ ⌨ ✕ ✓ ▶ ◀ ⚙ ⚠ ▸ ▾ ␣ Ⓐ 等）。
  - `xr_imgui_style.{h,cpp}`：
    - 启动面板用 shadPS4 卡片对话框的配色（深灰卡片、蓝色渐变标题栏、白色高光线、灰色次要文字、蓝色悬停和选中）；
    - 键盘用 shadPS4 输入法键盘的配色（底色 18、按键 35、功能键 60、回车特殊蓝 (30,90,170)、输入框 (22,37,60)）；
    - 手柄键位画成彩色圆形字母标签（A 绿、B 红、X 蓝、Y 黄）；
    - 焦点为淡白覆盖加 2 px 白色描边。
  - 启动面板：
    - 标题栏是"▶ 启动选项"、游戏名，显示方式放在右侧的胶囊里；
    - 选项为胶囊形，选中项打勾、蓝底，行间有分隔线；
    - 选项超过 3 个时用下拉框（用户要求，规则与 2D 面板相同）；
    - 底栏的取消、开始按钮带 B/A 键位标签和 ✕/▶ 图标；
    - 消息页带 ⚠ 图标；
    - 出现时淡入。
  - 键盘：
    - 模式键为"⌨ 中/EN"；输入框带光标，组字时文字加下划线；
    - 候选栏第一个为特殊蓝；
    - 退格、回车、Shift、空格用 ⌫ ⏎ ⇧/⇪ ␣ 图标；
    - 底部提示改为"键位标签 + 图标"，不依赖语言。
- **键盘位置**（用户要求：放低、倾斜，往下看刚好看到，方便操作）：像桌上的键盘，放在视线前下方，面板朝眼睛后仰（上沿远离、法线指向眼睛），宽 0.62 m、视线方向 0.75 m 远（约 45° 宽），中心在平视下方 32°。打开时按头的朝向放一次，之后固定在房间里，不跟头走；关掉再开，或 recenter（LOCAL 空间变化，和状态层一样）时重新放。
  - 2D 大屏时键盘跟着主面板，不跟头（用户要求）：打开时用眼睛相对大屏的位置算出键盘在大屏水平坐标系里的位置（水平居中在大屏正下方），之后每帧按大屏当前的位置和水平朝向摆放，转头不动，用侧握键挪大屏时一起走。大屏自己的俯仰不带进来。
  - 2D 大屏时还要让大屏下沿留在键盘上方：按眼睛看大屏下沿的俯角，加上键盘半高的视角，再加 2°，取它和 32° 中较大的一个，最多 45°。80″ 预设（高约 1.0 m、1.5 m 远）中心与眼睛同高时，下沿在下方约 18°，算出来约 31°，键盘仍为 32°。
  - 原来大屏时键盘竖直挂在大屏正下方、和大屏一样远（约 1.5 m），并跟着大屏移动；VR 时竖直放在头前 1.1 m、下方 0.32 m。
- **用手柄挪大屏**（用户反馈 OpenXR 手柄拖不动 Steam 大屏）：原来只有在菜单里打开"缩放/移动控制点"后，抓四角（半径 6 cm）或上下边中间的小条（16×12 cm）才行，1.5 m 外很难点中。现在侧握键在大屏上任意位置（含边外 5 cm）按住就抓起，手移动、前后推拉都跟着走，松开保存（与原来的移动逻辑相同）。扳机只负责点击和拖内容，侧握键不再算点击。游戏大屏要在指针模式下（双击摇杆）才生效，Steam 大屏一直生效。控制点和抓角缩放仍在菜单开关后面。
- **用扳机拖动 Steam 页面内容**（用户要求，与 2D Steam 页的修复相同）：Steam 大屏原来把射线当鼠标，扳机一按就按下左键，拖动时 Chromium 只会选中或拖动项目，不滚动。现在 2D 页的手势类提成共用的 `SteamTouchPointer`，射线的笔事件也走它：轻点是点击；拖动超过阈值（两倍 touch slop）后按起始方向发滚轮，内容跟着射线走：每格对应的拖动距离 = 屏幕高度 × 170/1080（1080p 下实测 Steam 的库和商店每格滚轮滚 170 像素；首页按行吸附），拖过半格就发，页面与射线的误差在半格以内；Steam 的平滑滚动保持开（`webhelper-instant-scroll` 文件可改为即时滚动，只作诊断）；按住不动 400 ms 才按下左键（滑块、拖放）。右摇杆滚动照旧。
- **Steam 大屏上的键盘图标**（用户要求，与 2D Steam 页相同）：页面右边缘、30% 高度处有键盘图标，射线点它打开或关上 XR 键盘（`SteamXrActivity.toggleKeyboard`）。原来键盘开着时射线和扳机全交给键盘，页面收不到，图标只能开不能关；现在原生键盘每帧报告射线是否在它上面（`XrKeyboardPointed`），不在键盘上的射线照常交给页面（也能点 Steam 的其他输入框），A/B 和摇杆仍只给键盘。射线从页面移到键盘时先松开页面上的按下；从键盘移回页面时，还按着的扳机或侧握键不算新的按下。
- 包：kb15 `25e67ad0…` 没打进词库（`assets.srcDir(任务)` 不会让 AGP 建立任务依赖），作废；kb16 `5dc7fd74…` 改用 variant API 的 `addGeneratedSourceDirectory`，词库在包里，审计按 SHA-256 核对通过。
- 上机后补的三处修正（kb17、kb18）：
  - 新流程的进程可能从没跑过 MainActivity，`ImmersiveXrActivity.onCreate` 补做 `ControllerManager.init` 和 `ContainerUtils.setContainerDefaults`；
  - Android 12+ 的 Noto Sans CJK 是 CFF2 可变字体，stb_truetype 不支持，改为优先用 Pico 的 `PICOSansSC-Regular.ttf`（TrueType），并先检查字体表；这个字体没有 Ⓐ Ⓑ ◀ ▶，提示改用普通字符；
  - 引擎的 `im_add_letter` 是空函数，改为每个字母对整段拼音调 `im_search`。
- kb18 `8eadb026…` 在 Swan 上通过：大屏面板、开始、游戏出帧、退出回到 Steam XR（不黑屏）、面板取消、拼音"你好"上屏送进 Steam。射线交互和 Alyx 还没测，见[记录](../validation/swan-official-steam-20261019.md)。
- kb25 `7b5ee3ed…` 在 Swan 上通过：XR 键盘的"你好"送进 Hades II 的 Wine（notepad 依次收到 `WM_CHAR` U+4F60、U+597D）；shadPS4 样式和下拉框见记录里的 kb20、kb21 截图。kb22 的单键位实现会把"你好"变成"好好"，kb23、kb24 的上限常量溢出，都已作废。
- kb26 `6f83dabc…`：X server 的 `GraphicsContext` 不再持有创建时的 drawable。Mesa X11 WSI 每个交换链漏一个 GC，原来每个漏掉的 GC 把约 8 MB 窗口内容钉在 Java 堆里，Steam XR 开着游戏约 18 分钟就 OOM；修复后同样场景 15 分钟堆平稳。见记录。
