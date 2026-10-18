# 官方 Steam 大屏 GPU 绘制：Thor 与 Odin 3 实测（2026-10-17）

分支 `feature/big_screen_rendering`，先基于 `malos/main` `8c15fe11`，后快进到 `e4a63690`（含主干“大屏 Steam 无法退出”的修复），含本轮未提交改动。设计见[规格 §11](../specs/xrgame-native-big-screen-rendering-v1.md)。

**结论（当天晚间，Odin 3 / A8xx）：GPU 模式可用，按 [规格 §12](../specs/xrgame-native-big-screen-rendering-v1.md) 的 L1 实施。** webhelper 的 GL（CEF 的 GPU 进程和 Steam 合成器）改走组件自带的 Zink → glibc Turnip，每个 Chromium 窗口经宿主 AHB 链在 Android X server 上有自己的覆盖窗口，按它在 Xephyr 中的层级排列。同一 MHW 详情页滚动时：

- 呈现率从 26.4 Hz 提高到 41.4 Hz，帧间隔中位数从 33.3 ms 降到 16.7 ms；
- 总 CPU 从 1.71 核降到 0.58 核。

GPU 模式仍默认关闭。白天在 Thor 上得出的“B-0 不适用”只对 ANGLE-Vulkan 和 llvmpipe 合成器成立，见下文“Thor 实测”。

## 构建环境

Windows 11 上的 WSL Ubuntu 24.04 x86_64，代替 Mac 的 Apple Container。用户要求所有输入都在本机自建：

| 输入 | 方式 | 结果 |
|---|---|---|
| 客户端 userspace | `ubuntu-base-24.04.3-base-arm64`（核对官方 SHA256SUMS）+ qemu-user-static chroot，运行 `build-rootfs.sh` | `linux-steam-rootfs.tar.gz` SHA `025a50c8…`，APK 内 `rootfs.tgz` `d2749a98…` |
| PRoot | `build-linux-steam-proot.sh`，NDK r27d | loader SHA `6f2ef63e…`，与 Mac 构建一致；`libproot.so` Build ID `220a30b1…` |
| Wine `lsteamclient` | `build-wine-client.py`，基于 xrg3（`9f3382ec…`） | `80477286…` |
| GPU 组件 | `build-gpu.sh`（Mesa `adb7e30a` + 6 个补丁，glibc/KGSL，交叉构建） | `libvulkan_freedreno.so` Build ID `83e8e2b1…`；`gpu.tgz` `398c9973…` |
| Turnip xrg16 | `build-turnip.sh` + `package-turnip-validation.py`，基线为 xrg13 `99a66963…` | `be6fbc76…` |
| DXVK xrg7 / VKD3D xrg4 | `build-wine.sh host` + `build-d3d.sh` + `package-d3d-quality-validation.py` | `dd6224a6…` / `d3ad4d76…` |
| OpenComposite | `build-opencomposite-clangcl.sh`：LLVM-MinGW 20250920，xwin 0.10.0 取 SDK 10.0.26100 与 CRT/ATL 14.44.17.14（用户同意接受微软许可） | `a51bbccf…`，四个补丁标记齐全 |

proton xrg3、FEX xrg5、imagefs `20260927-media1` 和 steamclient `validation9` 取自 Odin 3 上 10-11 版 APK 内的同名组件，哈希与原 catalog 一致。私有 catalog SHA `ba3db75b…`；Linux Steam staging manifest `12eaad0e…`。

## 与主干的偏差

- **OpenComposite pin 只在 WSL 测试克隆中修改**：PE 头的 TimeDateStamp 是链接时间（2026-10-17 09:35:56 UTC），配方未用 `/Brepro`，所以无法逐字节复现 Mac 的 `ce6bafc1…`。仓库里的 pin 未改动。VR 游戏不在本次范围。
- **Proton 仍是 xrg3**：Gradle 只检查 id `proton-11.0-2-arm64ec`。xrg3 不含 xrg5–7 的 Wine 补丁，也不含 `linux-steam1` 的 `lsteamclient` 路径修复。官方 Steam 内启动游戏不在本次范围。
- 各组件的哈希与 Mac 记录不同（Turnip `b3df0d95…`、DXVK `1e9efcab…`、VKD3D `feb565db…`）。它们是本机重建的候选，不继承 Mac 构建的设备验收。
- WSL 的 `build/xrgame-gfxr` 是 10-04 的旧暂存，按原样打进 debug 包。本次不验证 API 录制。

## APK

| 项目 | 值 |
|---|---|
| APK SHA-256 | `a6dc4598ac9471faa91024f8b4cc4aa1f725e82fa63f0015c98096d03722b178` |
| 审计 | `tools/audit-apk` 149 项、0 errors；原生 `RelWithDebInfo` |
| 签名证书 | `ce9d5b4d…`（本机 debug keystore，与设备上旧包一致） |
| `libextras.so`（新增 `hardwareBufferForSocket`） | Build ID `b2a5b34f326cb54e8f904b565bc5073ac52b292a` |
| `libwinlator_11.so` | Build ID `3144bab3fc41bc1eb68f9ec39d8ec751ac01c315` |
| `libxrgame_debugbus.so` | Build ID `4e6e6539cc582ff3943c892d8ec6a07532fcd45a` |

完整审计、构建日志和各组件记录保存在私有证据目录 `big-screen/v1/`。

## Odin 3（首个测试包）

AYN Odin 3，Android 15，`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`，SoC CQ8725S / Adreno 830，boot id `724e5140-b3b6-4f25-8ed4-530fdb622f76`。`/dev/kgsl-3d0` 权限为 `crw-rw-rw-`，`/dev/dma_heap/system` 为 `cr--r--r--`。

- 17:56 本地时间保留数据覆盖安装，取代 10-11 的旧包。DebugBus 报告 `gpuBundled=true`，`gpuRequested=false`（默认关闭）。
- 随后用 `linux_steam open` 开始软件基线，但前台被其他会话的 shadPS4 占用，用户要求暂停。`linux_steam stop` 后没有 PRoot/Steam 进程残留，日志目录尚未生成，没有任何测量数据。
- 幽灵进程相关的 `settings_enable_monitor_phantom_procs` 和 `max_phantom_processes` 都是默认值（读回为 null），未修改。

## Thor 实测

设备 AYN Thor，Android 13，`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`，boot id `b509848d-21a5-4f1d-b6ed-24c0572aa9d2`。原包备份在私有证据目录 `big-screen/thor-installed/`。

| 包 | APK SHA-256 | 说明 |
|---|---|---|
| v2 | `172b4d21bc3d0b492feac397de41c8ce0d6df5db24c244ea972649251e99d4e3` | 首个上 Thor 的包 |
| v3 | `1aa5492fbd9ae19064ab63583f98932f1f8fb497f33806bc82a53d1240bdc699` | 审计 149/0；Linux Steam manifest `5733d159…`，catalog `ba3db75b…`；`libextras.so` `b2a5b34f…`、`libwinlator_11.so` `3144bab3…`、`libxrgame_debugbus.so` `4e6e6539…`（与 v1 相同）；客户端 shim `a85f418d…` |

v3 的客户端 shim 含 ANGLE 参数注入（只在 GPU 模式下生效）。实测后已从源码撤回，所以当前源码与已装的 v3 在这一处不同。v3 重新 staging 后 `rootfs.tgz` 的哈希变了（`b6701549…`）：`stage.py` 用 `tarfile` 的 `w:gz` 写入当前时间，每次重新 staging 都会让设备重装 userspace。这是主干原有行为，本轮未改。

### 已验证

- glibc Turnip（KGSL）在 targetSdk 36 的 PRoot 中可用。宿主 AHB 呈现探针（`vk-x11-host-probe`，640×360）在 :0 上位置、颜色正确：v2 时 60–61 fps，另一个带 gallium/Zink 的构建（`HAVE_X11_DRM` 打开）62.0 fps。
- X server 的两处既有泄漏（DRI3 pixmap 未登记为客户端资源；Present 事件选择在断开或窗口销毁后残留）会让 CEF GPU 进程重启后报 `BadIdChoice`/`BadMatch`。v3 修复后，19:38–20:38 的 logcat 中 `XRequestError` 为 0，期间客户端多次断开重连并多次建立 swapchain。
- 软件基线（安装本分支之前的主干原包，MHW 详情页，SurfaceFlinger `--latency` 采样）：静止时 0 帧；方向键连续滚动时 13.55 Hz，帧间隔中位数 66.89 ms，P95 100.32 ms（截图 `thor-installed/t03-trunk-after-scroll.png`）。
- 软件模式：v3 恢复默认后，官方 Steam 大屏正常显示（`t40-v3-software-restored.png`）。

### GPU 模式的结论

1. **Steam 不转发 ANGLE 参数。** webhelper 命令行只有 `-cef-force-gpu` 对应的 `--ignore-gpu-blocklist`，没有 `-cef-use-gl`/`-cef-use-angle`。默认 ANGLE（GL 后端，经 Xephyr 的 GLX，只有 llvmpipe）下，GPU 进程 3 次 `exit_code=4`（GPU dead on arrival）后回退软件。
2. **GPU 进程由无沙箱 zygote fork 出来**，不经过 `exec`，按命令行找不到。
3. **用 shim 给浏览器主进程加 `--use-gl=angle --use-angle=vulkan` 后**，GPU 进程确实以 ANGLE Vulkan 启动，但浏览器主进程约 1.5 s 后死于 SIGILL，Steam 每 10 s 重启一次。崩溃栈在 webhelper 内 Valve 合成器线程（`SteamThreadTools::CThread`）调用的 Ubuntu llvmpipe（`libgallium-25.2.8`）JIT 代码里。只替换 argv、追加无害开关时，浏览器存活超过 20 s。所以是 GPU 合成改变了 Valve 合成器的 GL 路径，llvmpipe 才走到这段代码。
4. **大屏的显示结构。** Xephyr 窗口树里，大屏主窗口是 webhelper 用 SDL 创建的 OpenGL 窗口（`GLComposer`），Chromium 的 X 窗口挂在它下面；Valve 合成器用 `GLX_EXT_texture_from_pixmap`（栈中可见 `glXBindTexImageEXT` → `dri_set_tex_buffer2`）绑定 CEF 窗口内容再合成。Xephyr 没有 DRI3/glamor，GLX 只能走 drisw，绑定时从 Xephyr 读回像素（按 Mesa drisw 的实现推断，未逐帧测量）。所以 CEF 的帧即使绕过 Xephyr 送到 :0，合成器也读不到；要 GPU 化，CEF 窗口所在的 X server 本身必须有 DRI3 和 GPU 侧的 texture-from-pixmap。
5. **Zink 可行性（只在设备 userspace 中临时替换，未进包）。** 用同一 Mesa 树（26.3.0-devel，含调试跟踪）另建 Turnip + Zink + glvnd GLX/EGL。为不影响 Steam 主进程的 llvmpipe，以 glvnd 厂商名 `xrgame` 只给 webhelper 使用。Xephyr 上必须设 `LIBGL_KOPPER_DRI2=true`，否则报 “DRI3 not available”；设 `LIBGL_ALWAYS_SOFTWARE` 则报 “CPU device requested but none found”。webhelper 的 GL 切到 Zink 后：
   - Valve 合成器和 CEF GPU 进程都经宿主 WSI 建出 1280×720、4 张图的 swapchain，:0 上出现对应的覆盖窗口。
   - 随后浏览器主进程以 exit 1 退出，日志为 `Fatal IO error 11 (Resource temporarily unavailable) on X server :1`。栈在 Valve 合成器线程的 `glXBindTexImageEXT` → `XGetGeometry` → `_XFlush`。把每帧跟踪窗口几何的查询移到独立 xcb 连接后，结果不变。
6. cefsimple（无 Steam 合成器）在 Zink 下浏览器主进程 SIGTRAP，位置 `libcef.so+0x69afa6c`：`fcntl(fd, F_GETFL)` 后要求只读访问模式的 CHECK。原因未查明；DroidDeck 的 status shim 只对非 `steamwebhelper` 进程返回可写 memfd，可能相关（推断）。

失败与中止的记录、调试 shim、崩溃栈和截图都保存在私有证据目录 `big-screen/v2/`、`v3/`、`zink-test/`。

### 设备状态

GPU 模式已关闭（`gpuRequested=false`）。userspace 中的生产版 Turnip 已恢复（SHA `f848cae9…`，与 `gpu-build.json` 一致），临时放入的 Zink 库、glvnd 配置、调试 shim 和 `/data/local/tmp` 下的测试文件均已删除。Thor 仍装着 v3。

## Odin 3 实测（L1 实施）

设备 AYN Odin 3（Adreno 830，A8xx），Android 15，`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`，boot id `724e5140-b3b6-4f25-8ed4-530fdb622f76`。官方客户端版本清单 `1792192922`。测试中途用户在设备上登录了账号；凭据由用户输入，记录中没有任何账号信息。

| 包 | APK SHA-256 | 内容 |
|---|---|---|
| v4 | `48110a94a2a74852b4b48d58e44b608372529842b6efc1e9301a3bd745826111` | 只把 Steam 合成器换到 Zink，CEF 保持软件；`gpu.tgz` `75b79e4a…`，Turnip Build ID `703ee328…` |
| v5 | `0777d7e29ac99cf33cbd4af93234a13c78faad9a691a8be00e30b4e5cf1bc04b` | CEF 的 GPU 进程也走 Zink（`-cef-force-gpu`），覆盖窗口按 `_XRGAME_STACK_KEY` 排序。审计 149/0；Linux Steam manifest `eddb2c0a…`，catalog `ba3db75b…`；`gpu.tgz` `7345968e…`，Build ID：`libvulkan_freedreno.so` `f3b9332f…`、`libGLX_steampsp.so.0` `0abb961d…`、`libgallium-26.3.0-devel.so` `6ba41ecf…`；客户端 shim `c74d50d2…`；`libextras.so` `b2a5b34f…`、`libwinlator_11.so` `3144bab3…`（与 v1 相同） |

测法：SurfaceFlinger `--latency` 采样客户端页面的 SurfaceView 呈现时间；同时读取 app UID 下每个任务的 `/proc/<pid>/task/<tid>/stat`，得到各进程、各线程的 CPU（单位：一个核的百分比）。“滚动”指每 250 ms 注入一次方向键，4 次下、4 次上循环，采 20 s，CPU 统计窗口 18 s。

| 场景（MHW 详情页） | 呈现率 | 帧间隔中位 / P95 | 总 CPU | 主要线程 |
|---|---|---|---|---|
| 软件模式，静止 | 15 s 内 3 帧 | — | 7.7% | — |
| 软件模式，滚动 | 26.36 Hz | 33.34 / 50.01 ms | 171.1% | CEF `VizCompositorThread` 92.9%，Xephyr 36.8%，renderer 主线程 12.3%，光栅 7.0%，app 8.7% |
| v5 GPU 模式，静止 | 15 s 内 2 帧 | — | 7.6% | — |
| v5 GPU 模式，滚动 | 41.40 Hz | 16.67 / 33.34 ms | 58.0% | renderer 主线程 20.7%，app X 请求线程 7.9%，光栅 5.3%，`VizCompositorThread` 3.6%，Xephyr 0.8% |
| 调试 shim（E1，同一配置），滚动 | 41.66 Hz | 16.67 / 33.33 ms | 59.1% | 与 v5 相同 |

登录前的登录页一直有动画：软件模式下静止 29.71 Hz、总 CPU 215.4%（`VizCompositorThread` 98.8%，光栅 58.2%，Xephyr 37.4%）。

过程与结论：

1. **v4 是无效改动。** 只把合成器换到 Zink 后，登录页仍是 30.84 Hz、总 CPU 216.6%。逐线程统计显示，软件模式下 Steam 合成器所在的 webhelper 主进程只占 0.6%：大屏主画面是 CEF 画进自己的 X 窗口，合成器平时几乎不工作。真正的热点是 CEF 的软件显示合成和光栅。合成器为两个不可见窗口建的覆盖窗口一直未映射。
2. **E1（调试 shim）。** 去掉 `--disable-gpu`/`--disable-gpu-compositing`，让 webhelper 所有进程都走 Zink 并直出 :0。CEF 的 GPU 进程经 ANGLE（GL 后端）→ GLX（`steampsp` 厂商）→ Zink → kopper → 宿主 WSI 呈现，画面完整，CPU 降到约 0.6 核。问题：F1 菜单不显示。菜单是另一个 CEF 窗口，它的覆盖窗口被后创建的主视图覆盖窗口压住。
3. **v5。** WSI 在 Xephyr 上计算 surface 窗口的绘制顺序键（从根到窗口的兄弟序号），写到覆盖窗口的 `_XRGAME_STACK_KEY`；Java X server 让带键的窗口位于其他窗口之上，并按键排序。菜单正确显示在页面之上。菜单左上角的黑色区域在软件模式下也一样，不是回退。
4. 子进程识别：Chromium 经 `/proc/self/exe` 启动子进程，子进程的程序名不是 `steamwebhelper`。所以 shim 只在 Steam 启动的浏览器主进程里修改环境，子进程直接继承。
5. v5 运行期间 X server 的 `XRequestError` 为 0，没有 Xlib 致命错误，webhelper 没有循环重启。首页、详情页、滚动和菜单的画面与软件模式一致。

已知限制（未修）：

- GPU 模式下看不到鼠标指针：Xephyr 用 `-sw-cursor` 把指针画在自己的帧里，被覆盖窗口挡住了。手柄和触屏不受影响。
- 未测：从 GPU 模式启动游戏、经 Steam 电源菜单退出、长时间运行、Thor（A7xx）上的 v5、Swan 和 XR。

设备状态：Odin 3 装着 v5，GPU 模式开启（`gpuRequested=true`），停在游戏库。userspace 里的客户端 shim 已是 v5 包内的版本（SHA `41488f4a…`，与 manifest 一致）；实验用的调试 shim、脚本和 `/tmp` 配置已删除。逐次截图、日志和测量 JSON 保存在私有证据目录 `big-screen/odin3-v4/`、`odin3-v5/`。

### 启动过渡页（v7）

APK `d4b848a578d4bce0eb7b9deb563c5eed0ea9917ca67b6c31245f6a480f36a423`（审计 149/0，JVM 测试 53 项通过）。原先的过渡页只是一行静态文字；现在换成自绘的过渡页 `SteamLaunchView`：

- 背景柔光缓慢漂移，应用图标在不定进度环里呼吸；
- 状态文字切换时交叉淡入淡出；
- 三个阶段点：准备 → 打开客户端 → 载入大屏。服务新增 `LinuxSteamService.phase`，阶段不再靠比对状态文字；
- 最后一步超过 20 s 时淡入一行说明：首次启动或客户端更新时会久一些；
- 大屏画面就绪后淡出 420 ms，不再直接切走；
- 页面打开时，服务残留的初始“已停止”状态不显示。

Odin 3 上按约 2.5 s 一张截图验证了：没有英文状态闪现、阶段顺序正确、就绪后淡出。这次暖启动约 13 s 就进入了 Steam 自己的启动画面，所以 20 s 后才出现的说明没有在实机上看到。

### 重新进入大屏页面后黑屏（v8–v10）

用户报告：“正在载入”只显示约 1 秒，然后黑屏几秒，再直接出现 Steam 主页。v7、GPU 模式下录屏复现（私有证据 `big-screen/startup-v7a/`、`warm-v7a/`）：

- **冷启动正常。** 过渡页显示到就绪文件出现（打开后约 8 s），淡出后接上的是 Steam 自己的开机动画和“正在登录…”页面，中间没有黑屏。
- **Steam 已在运行时重新进入页面会黑屏。** 按返回离开大屏页面再打开：过渡页只闪约 0.3 s，之后 15 s 录像全黑；直到 Steam 因时钟分钟数变化重绘，主页才出现。

原因：

1. 每个大屏页面都新建一个 `VulkanRenderer`。GPU 模式下，覆盖窗口的画面经 Present 复制进渲染器自己的原生资源，不在 X server 的窗口内容里。新渲染器没有这些画面，而 Steam 空闲时不会再 Present，所以一直黑到 Steam 下一次重绘。
2. `onResume` 原本每次都重建整个渲染视图（本意只是重建输入队列），所以从游戏、后台回来也会走到同样的黑屏。
3. 旧渲染器的原生资源（Vulkan 设备、每个窗口的图像）从不释放：每进一次页面泄漏一份。
4. 新渲染器初始化完成之前，Steam 的 Present 会因为复制失败收到 BadAlloc。

修复：

- `PresentExtension` 记下每个窗口最近一次经硬件复制显示的 pixmap（`WeakHashMap`，窗口回收后自动消失）。`VulkanRenderer` 新建原生渲染器后，在 `updateScene` 之后调用 `replayShownFrames` 把这些帧复制进来；与 Present 在同一把 `renderLock` 下进行，所以总是最新的一帧胜出。
- 渲染器还没起来时，Present 以 SKIP 完成并记下这一帧，起来后由重放显示，不再回 BadAlloc。
- 大屏页面恢复时只重建输入队列，渲染器保留（surface 重建时原生资源本来就保留）。
- 页面销毁或客户端停止时调用新增的 `VulkanRenderer.release()` 释放原生渲染器。

JVM 单测无法构造 `Drawable`（静态块加载 `winlator_11`），重放逻辑只能在设备上验证。v10 APK `ce8a53328124bd3a80e15f4e5d8652d10b5b547a0c44b34f07885753552c1732`，审计 149/0，JVM 测试 58 项通过；Linux Steam manifest `a3446a34…`（`game-launch.py` 已更新），catalog `ba3db75b…`。

Odin 3 实测（同一 boot id `724e5140…`，GPU 模式开启，私有证据 `v10-*`）：

| 场景 | 结果 |
|---|---|
| 返回离开后点主页“Steam”卡片重新进入（新页面实例） | 点击后 0.05 s 起淡出中的过渡页下面已是重放出的主页，0.37 s 完整显示；没有黑帧 |
| Home 退到后台再从桌面图标回来（同一页面实例） | 约 0.1–0.2 s 直接显示主页，没有黑帧 |
| 冷启动（点“Steam”卡片） | 过渡页一直显示到就绪（装新包后要重新准备运行时，点击后 29 s），之后接 Steam 开机动画 |
| 进出页面后的显存 | EGL mtrack：冷启动后 81.7 MB，1 次进出后 46.3 MB，4 次后 56.4 MB，未见每次递增。第 5–7 次进出时设备断开，只有这 3 个点 |

冷启动后 Steam 自己的加载页（黑底 + Steam 图标）这次持续了 3 分多钟：登录 22:42:44 成功，主页 22:46:02 才开始渲染。`webhelper_js.txt` 记录 `async DynamicUserStore - 182,044ms`、`SteamApp Init - After Login total time: 182471 ms`，同时有 `AxiosError: Network Error`，`GetCMListForConnect` 的 Web API 调用 18 s 后失败（status 0）。这是 Steam 在等商店/Web API 的网络请求，不是我们的显示路径；官方客户端没有我们设的出站白名单。v7 那次冷启动主页在打开后约 31 s 渲染，差异来自网络。

其他观察：

- 首页（LaunchActivity）出现时没有状态栏，标题位置不再跳动。系统启动画面（应用图标那 0.35 s）仍带状态栏和导航条：那是系统为启动窗口画的，启动窗口不可聚焦，拿不到系统栏控制权，主题改不掉。触摸模式下截图里 Steam 卡片的默认焦点边框不明显。
- 录像里出现过的全屏雪花是 Odin 3 系统的“防残影像素刷新”（屏幕提示“防残影像素刷新进行中，点击退出”），不是我们的画面。
- 测试注意：主页在任务栈里时，DebugBus `linux_steam open`（服务里带 `NEW_TASK` 启动）只把已有任务带到前台，不会新建大屏页面；测重新进入要点主页的卡片。
- 未测：`linux_steam launch <appid>` 与桥接命令回传（设备断开）。

### 启动游戏：今天自建运行时的两处输入错误（v11、v12）

用户在 v10 上从官方大屏启动 MHW，提示启动依赖失败。两处原因都在今天 WSL 自建的运行时 bundle，与 v8–v10 的代码改动无关，v1–v10 全部受影响：

1. **catalog 缺字段。** 私有 catalog（`ba3db75b…`）是本机脚本拼出来的，只写了 `items`，丢了原 catalog 的 `version`、`updatedAt`。`ManifestData` 这两个字段没有默认值，解析失败，`XrGameComponents.load` 报 `Invalid XRGame component catalog`：Official 和 Custom 模式下任何游戏都过不了启动前检查。修复：生成脚本保留 `version: 1`、写 `updatedAt`；仓库的 `prepare-runtime-bundle.py` 现在直接拒绝缺这两个字段的 catalog。v11 APK `88b4aa122b3fdf7a9a829931f5d079d9584d53cf88e21752ae17611a20b0d2aa`，catalog `110f749b…`，审计 149/0，JVM 58 项通过。
2. **Proton 不是官方模式那一版。** v11 上 MHW 越过了启动前检查，又在 `XrGameOfficialSteam.prepare` 被拦下：“Install the official Steam runtime bundle before launching games.”。官方模式要求 Proton 的 `xrgame-build.json` 标明 `derivedFrom.patch = tools/xrgame/patches/wine-linux-steam-client.patch`，也就是此前官方验证用的 `linux-steam1`（xrg3 + 重编的 `lsteamclient.so`），而自建 bundle 放的是原始 xrg3。修复：用 `package-proton-validation.py --release linux-steam1`，以 xrg3（SHA `9f3382ec…`，与配方一致）和今天按 xrg3 编好的 `lsteamclient.so`（`wine-client-build.json` 的输入校验通过）重新打包，得到 `proton-11.0-2-arm64ec-linux-steam1.wcp` SHA `6210924561355ed2e3dc94b95587d8921c54efabe667327de910479895055c2d`（与 10-16 记录里的 `47676908…` 不同：`lsteamclient.so` 是今天在 WSL 重编的）。v12 APK `8ffb18a81fccd5f19ed94ca0b3f2ac67197bb2ee463785584000f9464946072e`，catalog `23d2f72e…`，审计 149/0。**v12 还没装上设备**（Odin 3 再次断开）。

v11 上的 `linux_steam launch 582010`（22:59，会话检查已为 0）：转交程序 5 s 内返回 0；Steam 自己处理约 45 s 后经兼容工具发出请求，`OfficialSteamGameActivity` 到前台。Steam 的命令行是 `launch.py waitforexitandrun '<游戏库>/common/Monster Hunter World/MonsterHunterWorld.exe'`，桥接请求为 `{"appId": 582010, "pid": …, "command": {"executable": "MonsterHunterWorld.exe", "arguments": [], "workingDirectory": "."}}`，与 JavaSteam 缓存的启动配置一致（`MonsterHunterWorld.exe`，无参数）。之后卡在上面第 2 点，游戏没有启动。

另一个问题（未修）：Android 侧启动失败后，如果用户按 Home 离开而不是在启动页按返回，`finishGame` 不会执行，桥接一直等到 5 分钟超时，期间 Steam 认为游戏仍在运行。

v12 上（同一 boot id，23:11–23:20）：

- **官方模式：游戏能启动，但被系统杀掉 Steam 后退出。** `linux_steam launch 582010` 转交成功，约 50 s 后桥接交接，`steam.exe` 和 `C:\xrgame\games\582010\MonsterHunterWorld.exe` 启动（MHW 兼容配置的目录别名照常生效），MHW 窗口出现。23:14:49 系统日志 `Killing PhantomProcessRecord {…libproot.so…}: Trimming phantom processes`：Android 的幻影进程（子进程）限制杀掉了官方客户端的一个 PRoot 进程。MHW 随即报 `fatal stalled cross-thread pipe (pipe is disconnected)` 退出，官方服务停止，桥接收到 `failed`。原因是 Wine 的十几个进程加上 PRoot 里 Steam 和 webhelper 的进程超出了系统对 app 子进程的上限。可由用户在开发者选项里关闭子进程限制（各 ROM 叫法不同），我们没有改系统设置。
- **Custom（JavaSteam）模式：MHW 正常启动。** 经主页“其他”切换（官方服务停止，JavaSteam 运行、已连接、已登录），DebugBus `launch 582010` 后约 30 s 出现 MHW 进程，用户确认游戏已启动。这也证明 v12 的 catalog 修复对 Custom 模式有效。
- 用户决定：暂时用 JavaSteam 模式玩游戏；大屏（官方模式）启动游戏也暂时回到之前耦合 JavaSteam 的方式（此前已验证能进游戏、领奖励）。`XServerScreen` 已恢复原样，Steam 的命令只在 DebugBus `steamCommand` 里显示。注意：幻影进程限制与启动信息来源无关，官方模式下用旧方式启动同样可能遇到。
- v13（回退启动信息来源）：APK `61883d1ecbf0fc7c77fe1574b89910c49fbe85da6fb3e2cef5d1ea082321f1d6`，审计 149/0，JVM 58 项、Python 33 项通过；catalog `23d2f72e…`（同 v12），Linux Steam manifest `f59a270b…`（`game-launch.py` 注释更新）。之后安装到 Odin 3 复测（23:34–23:39）：官方会话 23:35:11 登录，`linux_steam launch 582010` 后 Steam 约 110 s 才经桥接交接，`steamCommand` 显示 `MonsterHunterWorld.exe`；`XServerScreen` 用 JavaSteam 的 `appLaunchInfo` 启动，MHW 进程出现。23:38:05 系统再次 `Killing PhantomProcessRecord {…libproot.so…}: Trimming phantom processes`，这次杀的是 PRoot 根进程，Xephyr、steam、8 个 steamwebhelper 随之退出，MHW 报 `fatal stalled cross-thread pipe` 退出。**所以官方模式启动游戏失败与启动信息来源无关**，原来耦合 JavaSteam 的方式同样会遇到。设备设置（只读）：Android 15，`max_phantom_processes` 未设置（默认 32），`settings_enable_monitor_phantom_procs` 未设置（默认开启），当时已有 20 个幻影进程；此前通过的官方模式游戏验证是在 AYN Thor 上做的。
- 用户自己关闭子进程监控后复测（`settings_enable_monitor_phantom_procs=false`，23:47–23:51，仍是 v13）：会话 23:47:56 登录，`linux_steam launch 582010` 后 5 s 交接。MHW 依次显示中间件 logo、选择存档（3 个原有角色都在）并开始加载存档，约 2.5 min 内 MHW 和官方 Steam 的 9 个进程都在，23:48 之后没有幻影查杀。用户确认可以玩；23:50:33 用户在多任务界面划掉了应用，MHW 随之结束（`LAUNCHER_TASK_DISMISS_SWIPE_UP`），不是崩溃。没测更长时间的游玩。

### 2026-10-18：GPU 绘制默认打开，官方启动改用 Steam 的命令（v14）

用户要求：大屏 GPU 绘制改为默认打开；再出一版不用 JavaSteam 启动信息的包复测。

- `LinuxSteamGpu`：开关文件写 `on`/`off`，没有文件时视为打开，只有明确 `off` 才关（`linux_steam gpu off` 写入 `off`）。
- 官方启动重新先用桥接传回的 Steam 命令解析目标（即 v8–v12 的做法），缺失或不可用时回退到 JavaSteam 缓存；兼容配置照旧提供启动器选项。游戏元数据和入库记录仍来自 JavaSteam 缓存。
- v14 APK `72ba6e5e20e8b6fce15024cd4c70bce1551640ec85e35510f9826db6c1bcd471`，审计 149/0，JVM 59 项、Python 33 项通过；catalog `23d2f72e…`（同 v12），Linux Steam manifest `33f73d10…`。已保留数据覆盖安装到 Odin 3（同一 boot id，子进程监控已由用户关闭，GPU 开关文件仍是 `on`），**等待用户复测**。

