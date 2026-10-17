# DroidDeck 调研与 SteamPSP 路线复核

首次调研：2026-10-13；整理更新：2026-10-15。状态：**源码调研与待评审建议**。

本文供独立 AI 或工程师复核三个决策：是否引入官方 Linux Steam、哪些 DroidDeck 实现值得局部借鉴、是否值得接通 SteamPSP 的 AHB 直呈。先读“核心结论”和“独立复核清单”；需要评估显示改造时，再读 [AHB 减拷贝与 scanout 专项](../specs/xrgame-native-ahb-scanout-evaluation-v1.md)。两份文档一起交给评审者即可，不需要之前的聊天记录。

当前建议是保留 Bionic/XR 主链，优先补齐可观测性并评估已有显示路径；官方 Steam 是另一项服务兼容性决策。请重点质疑这项建议及其源码依据，而非默认接受。本文不变更 [personal-v2](../specs/xrgame-native-personal-v2.md) 的执行顺序或验收标准。

## 对照版本与范围

- SteamPSP：`feature/malos/swan-xr-ahb`，`dc877ba12dfe3ad32138c38b3f8bf33e64e72142`。
- DroidDeck 上游：[Droid-Deck/DroidDeck](https://github.com/Droid-Deck/DroidDeck)，维护镜像：[tencentmalos/DroidDeck](https://github.com/tencentmalos/DroidDeck)。已先 fork，再作为 `references/DroidDeck` 浅克隆子仓引入。
- 固定版本：[`2503d6929332de2a4a22c8a31c1b3ce84e5a8831`](https://github.com/Droid-Deck/DroidDeck/commit/2503d6929332de2a4a22c8a31c1b3ce84e5a8831)，上游 `main`，提交时间 2026-10-12 21:13:07 UTC，包含 Quest 3/3S 标准 APK 兼容改动。它晚于 0.4.0 release，不能把两者当成同一制品。
- 本次只读源码、配置和上游文档；未运行 DroidDeck、未做同设备游戏 A/B。以下“已有”指源码实现，性能收益及设备兼容性均需另验。
- 10 月 15 日整理沿用上述 DroidDeck/SteamPSP 固定版本，**不是对上游最新提交的重新验收**。Steam 客户端及 Steam 下载的 Proton 还有各自的动态版本，不能由 DroidDeck gitlink 唯一确定。
- DroidDeck 主仓标注 GPL-3.0，组件按各自许可证处理。本次不复制实现，也不使用其 runtime、APK 或签名材料；不改变 SteamPSP 的组件 pin。

## 核心结论

两者都用 Wine/Proton、FEX、DXVK/VKD3D 和 Turnip，主要区别在它们外面的运行环境与显示/服务链路。

DroidDeck 将完整 Linux Steam 客户端、gamescope 和 ARM64 Proton 放进 glibc rootfs，由 PRoot 运行；SteamPSP 由 Android 前端管理 Steam，再直接启动适配 Bionic 的 Wine/ARM64EC/FEX。**DroidDeck 不是用更少的 Linux 依赖实现相同功能，也不是“全部 x64 模拟”对“我们原生 ARM64”这么简单：它的 Windows 路径同样使用 ARM64EC Wine 与进程内 FEX。** [D1][D2]

建议保留当前 Bionic/XR 路线，优先借鉴它的呈现路径诊断、完整会话状态和输入恢复机制。**完整 Steam 客户端的主要价值是服务兼容性与维护分工；AHB 呈现的价值是潜在的显示成本下降。二者可以分别评估。** 是否引入完整 Steam 客户端，应作为独立兼容性路线评估；它不会自动解决所有 DRM、反作弊或 Android 执行限制。

后续专项复核见 [AHB 减拷贝与 scanout 实施前评估](../specs/xrgame-native-ahb-scanout-evaluation-v1.md)：补充 `isDirectScanout()` 固定 false、picoXr Present 分支提前处理 AHB、scanout 显示归还通知缺口，以及 AI LIMIT 的对照方案。该文档待独立评审，尚未实施或进行新设备测试。

## 主流程

下面表示职责和主流路径，不表示所有步骤在同一进程，也不代表每帧都走满全部分支。

```text
DroidDeck
Android UI / SessionService
  → PRoot + glibc ARM64 rootfs
  → gamescope / Xwayland + 完整 Steam ARM64 客户端
  → Steam 选择的 ARM64 Proton + 兼容启动包装
  → Windows 游戏：ARM64EC Wine + FEX + DXVK/VKD3D → Turnip
  → gamescope / Wayland → Android 原生 Wayland compositor
  → 可用时 AHB / SurfaceControl；否则 GPU 拷贝或合成 → Android 显示

SteamPSP
Android Compose + JavaSteam / libgndownload / Cloud
  → 每游戏 prefix + BionicProgramLauncher + 已固定的 runtime
  → Windows 游戏：Bionic ARM64EC Wine + FEX + DXVK/VKD3D → Turnip
  → X11 / DRI3 / Present → app 内 XServer → AHB / Vulkan 显示
  → 普通 Android Surface，或 SBS / XR 影院

SteamPSP PCVR 分支
OpenVR 游戏 → OpenComposite → Windows OpenXR runtime / Android bridge
  → SBS 模拟或 Swan OpenXR → 眼图像重建与 XR swapchain
```

DroidDeck 还有 Linux 原生程序、x86 Linux FEX、桌面等分支；其 Windows 包装器移除 `require_tool_appid`，避免 Steam 默认 pressure-vessel 因 Android 不提供相应 user namespace 而导致启动失败。它仍有自己的环境、音频和同步适配，并非原封不动执行桌面 Steam。 [D1][D2]

## Steam 的来源与开源边界

DroidDeck 使用 **Valve 官方 Linux ARM64 Steam 二进制客户端**，不是自行实现的开源 Steam 客户端。固定版本的 [`droiddeck-steam-install`](https://github.com/Droid-Deck/DroidDeck/blob/2503d6929332de2a4a22c8a31c1b3ce84e5a8831/tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-install) 从 Valve 的 `client-update.fastly.steamstatic.com` 获取 `steam_client_steamdeck_publicbeta_linuxarm64` manifest，选取通用及 Linux ARM64 包，校验 SHA-256 后安装，并启动 `steamrtarm64/steam`。它提供下载/启动适配，不提供 Steam 客户端本体源码。 [D11]

Valve 的 [steam-for-linux](https://github.com/ValveSoftware/steam-for-linux) 是客户端问题跟踪仓库；[Steam Runtime](https://github.com/ValveSoftware/steam-runtime) 和 [Proton](https://github.com/ValveSoftware/Proton) 的开源代码属于运行环境与兼容层，不能据此把 Steam 客户端本体视为开源。DroidDeck 的 GPL 许可也不覆盖 Valve 客户端。

这里能确认的是安装脚本指向官方 ARM64 客户端包；**不能据此声称 Valve 支持 Android、PRoot 或我们的 Bionic/XR 组合**。未来若做原型，须另记实际下载的 Steam manifest、客户端版本及包哈希，以及 Proton depot/build；同一个 DroidDeck 提交不保证每次得到同一运行时。

## FEX 在哪里执行，和 shadPS4 是否相同

| 路线 | ARM64 Host 侧 | 仍需翻译的 x64 部分 | 不能据此推出的结论 |
|---|---|---|---|
| DroidDeck Windows 路线 | Android 前端/Wayland compositor、Linux ARM64 Steam、ARM64EC Wine 的原生部分 | Windows x64 游戏代码及未被原生组件替代的 x64 DLL；使用 Wine 进程内 FEX | Steam 是 ARM64 不代表游戏没有 FEX 成本；独立 Linux x86 FEX 路线也不能混作同一条链 |
| SteamPSP | Android 前端、Bionic Wine 的 ARM64 原生部分、已捆绑 ARM64EC 图形组件及 Android 显示端 | Windows x64 游戏代码及其余 x64 DLL | 当前 JavaSteam/Android UI 本来就不经过 FEX，因此换 ARM64 Steam 没有“消除原有 x64 Steam 翻译”这项收益 |
| shadPS4 Android Host 路线 | ARM64 host core、HLE 和 renderer | PS4 x64 guest；另有有意编译为 x64 的同步/Gnm 小型 guest payload | 不是把整个 shadPS4 编译成 x64 再由 FEX 执行 |

DroidDeck 的 Windows 边界见 `steam-compatibility` 和启动包装器 [D2]；SteamPSP 组件边界见现有系统设计与 runtime 说明 [S1][S6]。shadPS4 结论来自 10 月 15 日核对的独立工作树 `39b449360111faa213b17bc80365abd0750e1c50`：Android 构建指定 `arm64-v8a`/`BUILD_HOST_CORE=ON`，FEX syscall handler 接 HLE，CMake 另生成小型 guest payload [X1]。**这个工作树版本不是本仓 `references/shadPS4` 的旧 gitlink**，此次未推进该子仓。

ARM64EC/FEX 桥和 shadPS4 HLE 都能让部分工作在 ARM64 Host 执行，但前者保留 Windows ABI/兼容语义，后者由模拟器实现 PS4 服务语义。它们有可比较的跨边界成本，不是可互换实现；不能仅凭“用了同一 FEX”预期同等性能。

## 主要差异

| 方面 | DroidDeck 固定版本 | 当前 SteamPSP | 对我们的意义 |
|---|---|---|---|
| Steam 服务 | 运行完整 Steam Linux 客户端，复用其安装、Cloud、控制器和兼容工具流程 | JavaSteam、Rust 下载器、自己维护 Cloud；Windows 客户端 API 由已捆绑 GBE loader 路径提供 | DroidDeck 少维护一套 Steam 协议/界面逻辑，但带入 Steam/CEF 进程与更新行为；不能据源码断言它更快或游戏全兼容 |
| 执行环境 | glibc rootfs + PRoot；Windows 用 ARM64EC Proton/FEX；另外支持 Linux 应用 | Android/Bionic Wine + ARM64EC/FEX；imagefs 是 runtime 文件树，不是运行中的 Ubuntu 系统 | 换成 DroidDeck 路线属于运行环境迁移，不是单独替换 X server |
| Android 目标 | `compileSdk 34`、`minSdk 26`、**`targetSdk 28`**；源码明确其 rootfs 执行方式依赖该目标域 | picoXr `minSdk 29`、**`targetSdk 36`**，已有 app-domain 执行适配 | targetSdk 低不等于不能装新系统，但不能拿它证明 targetSdk 36 路线已解决；需重新验证 SELinux/W^X/子进程限制 |
| 窗口系统 | gamescope/Xwayland + app 内原生 Wayland compositor；也有直接 Wayland 桌面路径 | app 内 XServer 与 DRI3/Present，连接 Wine X11 | Wayland compositor 在 Android 原生侧，不表示整个运行链摆脱 Linux；换协议本身无 FPS 保证 |
| GPU 呈现 | `banner_ahb_v1` 接收游戏 AHB，可直交 SurfaceControl；另有一次 blit 和完整合成分支 | 已验证基线是 Turnip X11 AHB GPU 拷贝/可选异步 Present；另有直接采样实验和继承的 SurfaceControl scanout 源码 | 优先比较 fence、buffer 生命周期和回退原因，避免再引入一套大 compositor |
| 音频 | Steam 客户端经 PulseAudio 内置 DirectAudio sink；游戏 DirectAudio 默认开启，经 Wine driver/Unix socket relay 到 Android | Wine → PulseAudio → AAudio；未接 Foundation 音频 | 有可参考的短路径，但不是本次接入范围，也不能直接复用其 glibc 制品 |
| 手柄 | fake evdev/hidraw、Steam Deck 输入适配、Steam Input；共享 ring + 全状态快照恢复 | Android → evshim 共享状态 → SDL/winebus/XInput；VR 另有 OpenXR action 分发 | 可对照丢事件、热插拔和焦点切换的恢复语义，不宜再叠加一层 Steam Input |
| 键盘 | X11 游戏用隐藏 Android editor 转 key events；直接 winewayland 路径另有 text-input-v3 | 隐藏 `IMEInputReceiver` → XServer 按键；有 composing 与过期 InputConnection 防护 | 它也不能仅凭 Steam/game 窗口就自动知道所有游戏的文本焦点；完整 IME 协议是另一项改造 |
| 可分析性 | ADB JSON provider、会话阶段、首帧、gamescope 焦点、脚本场景、失败证据、Steam CDP | DebugBus、Litep、native/guest debugger、GFXReconstruct，且已有游戏专用状态解析 | 它的端到端编排值得借鉴；这些能力不能替代我们的 guest 指令/线程调试与 API replay |
| XR | Quest 3/3S **仅 2D 面板**；文档明确未加入 immersive VR/OpenXR | SBS、影院、OpenComposite/OpenXR、Swan Vulkan composite 与 SGSR/FSR1、眼动 FDM | DroidDeck 不是现成的 PCVR/Swan 替代方案；我们也仍需保留原有长玩/控制器验收边界 |

依据：DroidDeck [D1]–[D11]；SteamPSP [S1]–[S6]。

## 用官方 Steam 替换 JavaSteam，实际需要替换什么

“替换 JavaSteam”至少包含两个不同边界。Android 侧的账号、库、下载和 Cloud 是一层；Windows 游戏的 `steam_api`/`steamclient` 调用接到哪个客户端，是另一层。当前 GBE loader 提供的游戏侧接口不会因为启动了官方 Steam 自动改接过去。 [S1]

```text
当前两条职责链
Android 前端 → JavaSteam / libgndownload / Cloud RPC → Valve 服务
Windows 游戏 → 捆绑 GBE loader / Steam API 兼容实现

官方客户端路线要证明的链
Windows 游戏 → Proton Steam API 桥 → Linux Steam 客户端 → Valve 服务
Android 前端 → 会话/安装/启动控制接口 ────────┘
```

下表是待评估范围，不表示桥已实现：

| 范围 | 可能获得的价值 | 必须解决的接口与代价 |
|---|---|---|
| 仅引入官方客户端管理库/下载/Cloud | 少维护部分服务协议与桌面流程 | Android UI 如何取得状态、启动与失败信息；账号和保存路径归属；避免两套下载器/Cloud 同时写；游戏内 Steam API 仍未因此接通 |
| 官方客户端接通游戏侧 Steam API，保留 Bionic Wine | 潜在减少 DLC、回调、账号、成就等自维护兼容逻辑 | `lsteamclient` 一类桥、进程发现/IPC、位数与 ABI、客户端生命周期；须证明 Bionic Wine 和 glibc Steam 能协作 |
| 完整采用 glibc Proton + PRoot + Steam | 更接近 DroidDeck 已组织好的服务/启动环境 | targetSdk 36 执行限制、PRoot/CEF 资源成本、现有 runtime/prefix/存档迁移、DebugBus/调试链和 Swan/XR 重新适配 |

**关键未决项是 Bionic ↔ glibc 的游戏侧桥接。** 不能把官方 glibc `steamclient.so` 当成普通 Android SDK 库直接链接。是适配现有 Proton 桥、设计进程间桥，还是保留独立 glibc 兼容环境，需要先确定 ABI 和 IPC 边界；目前没有本项目的实验证据。官方客户端能运行，也不等于 Windows 游戏已经使用它。

如果未来单独批准探索，最小原型应证明：一个合法拥有的无反作弊游戏能通过官方客户端启动、游戏侧账号/所需 Steam API 回调正确、保存路径与 Cloud 往返正确、正常退出并可重启；再测启动耗时、内存/CPU、断网/客户端更新和 targetSdk 36 行为。完成这些以后，才能决定是否替换 GBE 或扩大游戏集合。现有 Bionic 运行链、安装目录和存档应保留为对照。

因此，“完整 Steam 是主要值得评估的架构差异”是合理判断；“其余替换都无意义”则过强。显示的 fence/回收、状态机、输入恢复可独立借鉴，无需迁移整个运行环境。

## 呈现：值得细看，但不能只比较“零拷贝”这个名称

`ahb_swapchain.c` 和 `compositor.c` 已有真实实现，不只是一份 `ZERO_COPY_SPIKE.md` 设想： [D3]

1. 游戏通过私有 Wayland 协议关联 AHardwareBuffer 与 `wl_buffer`，Android 侧持有对应引用。
2. 可直呈时导出 DMA-BUF acquire fence，交给 SurfaceControl 等待；收到显示 release fence 后导回 DMA-BUF 或挂入事件循环，再释放客户端 buffer。
3. 没有合适 AHB、层不可用或需特效/帧生成时，走 blit/合成分支。源码分别统计 copy、layer、zero-copy frame，并记录回退原因。**layer frame 不一定是 zero-copy frame。**
4. 仍有边界：acquire fence 导出失败时，源码会 `poll` DMA-BUF，单次超时 100 ms。因此不能将其整体描述为“呈现线程永不等待”，也不应直接照搬失败处理。

这里省的是应用侧的图像拷贝/合成；SurfaceFlinger 是否最终交硬件显示层或再 GPU 合成，是另一个问题。更不能从这条 Android 2D 通路推导出 XR 眼图像重建、畸变/合成也可全部省掉。

我们已有 [`VulkanRendererScanout.cpp`](../../app/src/main/cpp/winlator/VulkanRendererScanout.cpp) 和 [`VulkanRenderer.java`](../../app/src/main/java/com/winlator/renderer/VulkanRenderer.java) 的 SurfaceControl 路径，不能说“完全没有”。但该基线 `nativeMode` 初始为 false，主源码未找到外部 `setNativeMode` 调用；不能把存在的代码当成 picoXr 正在使用、已验收的直呈。SBS 影院还明确禁止 nativeMode 绕过影院 quad。另有 `surfaceflinger` 配置进入独立 ASurfaceRenderer，具备转换 buffer pool 的显示回收，但源 Guest AHB 直呈仍缺 display release→X11 Idle 闭环；详见上述专项评估 §3.3。现有异步 Present 见 [S2]。

对 AI LIMIT/MHW 更合理的下一步是：先把当前路径的 copy 耗时、queue residence、acquire/release 等待、回退原因与实际显示帧关联，再决定是否接通/修正已有直呈路径。异步等待和零拷贝解决的是不同成本，不能互相替代。

## Fast path：借鉴方法，不移植错误层级

DroidDeck 的 `ProotFastPath.kt` 已接入 SessionService，偏好默认开；前提是使用它自己打过补丁的 PRoot、seccomp 可用且 root/bind 描述满足限制。`libblfastpath` 在进程内处理一部分路径映射，经 seccomp 放行的 trampoline 发出调用，减少 PRoot tracer 的往返。root/bind key 用来避免继承环境后在另一个 PRoot 实例中错误转换路径。 [D4]

它优化的是 **PRoot 的路径/系统调用转换**。我们没有这层 tracer，直接搬来不会消除 Wine 的对象等待、FEX 的翻译开销，或 XServer 的呈现串联。它与 shadPS4 的 guest HLE、FEX Windows ARM64EC 桥也不是同一个机制。

可借鉴：快慢路径语义对照、明确的环境指纹、逐调用类型计数、失败回退、可关闭的 A/B。其 `proot-performance.md` 的微基准是上游设备/方法结果，本次不换算为 SteamPSP 的预计 FPS。

同步方面，DroidDeck 为不同 Steam Proton 版本选择配套 ntdll/wineserver sync pack，并记录 esync/fsync/ntsync 相关设置；我们已维护 `ntsync-android` 会话路径。可参考其版本匹配和诊断输出，不能把同名同步方案或替换 DLL 当成无条件提速。 [D10][S1]

## 输入与诊断的可落地点

**输入恢复。** `FakeInputWriter` 使用固定 mmap ring、递增事件序号和 seqlock 保护的绝对状态快照；native reader 可在新打开、溢出后恢复当前按键/轴状态。我们应先验证现有 evshim 在失焦、重连、暂停/恢复后的行为，再补齐缺口，而不是因传输形式相似重写整个输入系统。 [D5][S3]

**IME 分清两条链。** `KeyboardHost` 给 Steam/gamescope X11 客户端发按键，`wl_text_input.c` 则向 winewayland 发送 preedit/commit 和焦点信息。后一条更接近真正文本协议，但不能直接套到我们的 Wine X11 路径。我们的近期修复已有显式显示键盘、composing 缓冲、连接 generation 与 7 项单测；[设备验收记录](../validation/mhr-ime-20261011.md) 的未完成项仍需复测，不因这次源码对照而关闭。 [D6][S3]

**统一会话证据。** DroidDeck `state` 同时包含 phase、firstFrame、focusedApp、focusableApps 和失败信息，场景工具支持等待条件、断言、finally 清理及失败截图/日志。与“进程还活着”相比，这更适合定位“启动了但黑屏”“窗口焦点丢失”“键盘没有落到游戏”等问题。我们可沿用现有 DebugBus 增加统一 session/run id 和这些观测，串起 Litep、日志、窗口和首帧，而不新造一套远程服务。 [D7][S4]

## 建议顺序

| 优先级 | 后续候选工作 | 验证方式 |
|---|---|---|
| 1 | 给现有 DebugBus/Litep 补齐“点击启动 → runtime 就绪 → guest 进程/窗口 → 首次 Present → 实际显示 → 焦点/输入”的统一关联 | AI LIMIT/MHR 正常启动、故意失败与暂停恢复；不能只以 PID 或首个提交为成功 |
| 2 | 审计当前 AHB 所有权、fence 与已有 scanout 接通条件；输出每帧实际路径和回退统计 | 同设备、同场景、同会话 A/B/A；看 frame time、排队和等待，再看平均 FPS；保留图像正确性与退出/重建检查 |
| 3 | 输入全状态恢复、IME 显隐/焦点/重连场景测试 | 物理手柄、软键盘、切前后台与窗口切换，确认无卡键或丢轴 |
| 后续 | 若 Steam 服务兼容性成为主要障碍，另评估完整客户端路线；音频直通、完整 Wayland 文本协议分别评估 | 单独定义 targetSdk 36、资源开销、Swan/XR 接口与游戏兼容性门槛 |

这些是评估建议，不是本次新增的已实施工作包；当前 personal-v2/spec 和设备验收顺序仍有效。

## 独立复核清单

| 待复核判断 | 当前证据和边界 | 哪种证据会改变决策 |
|---|---|---|
| 官方客户端的价值主要是服务兼容性，而非直接提 FPS | 安装/启动脚本和双方职责对照；没有同设备客户端资源测试 | 证明现有 Steam 服务/GBE 才是具体游戏的主要阻碍，且官方桥确实解决；或证明新客户端资源成本不可接受 |
| 保留 Bionic Wine 时，接入官方 Steam 不是单独换 Java 依赖 | glibc/Bionic 和游戏 API 边界明确，实际桥仍未验证 | 已有可复现的兼容桥与版本条件，而非仅有桌面登录截图 |
| 可复用已有 AHB/SurfaceControl 路径，暂不迁移 Wayland | 两种 renderer 都有源码入口；Guest buffer 的显示归还仍有缺口 | 找到遗漏的有效 release→Idle 实现，或设备证据表明现有分配/同步协议无法满足直呈 |
| 先测显示关键路径，再选择减拷贝实现 | 历史 AI LIMIT 的 copy elapsed 改善曾只是等待转移，详见专项 §2 | 同场景 GPU copy/compose 时间、串联等待和独立显示帧数据证明可回收的帧时间 |
| DroidDeck PRoot fast path 不适合直接移植 | 当前 SteamPSP 没有对应 PRoot tracer | 识别出真正共享的热点及调用边界，并能用独立测量验证；不能只比较 syscall 名称 |
| 输入恢复与会话证据适合局部借鉴 | 源码中存在状态快照、阶段/首帧和场景断言 | 现有 evshim/DebugBus 已覆盖相同语义，或新增观测成本超过其诊断价值 |

请输出以下内容，允许结论为“暂缓”或推翻本文建议：

1. **事实纠错**：指出具体文件/符号/固定提交，将源码事实、作者说明、推断和运行证据分开。尤其检查 ARM64 Steam、ARM64EC/FEX、ASurfaceRenderer 与 Vulkan scanout 的边界。
2. **路线判断**：分别给“局部借鉴”“官方 Steam 独立兼容路线”“整体替换”评价；不要把采用官方客户端与迁移显示协议绑定。
3. **最小验证**：每项列出需要补的数据、涉及组件、可回退方式，以及何种结果应停止。没有测量时不报预计 FPS 百分比。
4. **显示专项审阅**：核对 producer-ready、consumer-release、X11 Complete/Idle、fallback 和 surface 重建；用普通非 SBS 的 AI LIMIT 作为未来性能目标，按专项的 A/B/A 或 ABBA 方案评估。

可直接复制给另一位 AI：

```text
请独立复核 docs/background/droiddeck-comparison-20261013.md 与
docs/specs/xrgame-native-ahb-scanout-evaluation-v1.md。
目标是判断 SteamPSP 借鉴 DroidDeck 的哪些部分值得实施，先不要写运行时代码或接管设备。
以文档固定提交的源码为准；若发现更新的实现，请另列版本差异，不混用基线。
重点检查：官方 ARM64 Steam 的来源及服务/游戏 API 两层接入范围；
Bionic Wine 与 glibc Steam 桥接的可行性；FEX 真正的翻译边界；
现有 VulkanRenderer 与 ASurfaceRenderer 的 AHB 接通、fence、buffer 归还条件。
区分已有代码、历史实测和待验证假设，不把“零拷贝”或 CPU/GPU 未满直接等同于提速。
请给出事实错误、支持/反对的路线、最小实验和停止条件，并列源码位置。
源码或设备数据不足时明确缺什么；不要把未运行的实验写成结果。
```

## 源码与证据索引

以下 DroidDeck 相对路径均位于上述固定子仓版本，不跟随上游最新分支。已有主仓 checkout 时仅需 `git submodule update --init --depth 1 references/DroidDeck`，不使用 `--remote` 或递归初始化。

没有本地子仓的评审者可直接浏览 [DroidDeck 固定源码树](https://github.com/Droid-Deck/DroidDeck/tree/2503d6929332de2a4a22c8a31c1b3ce84e5a8831) 和 [SteamPSP 固定源码树](https://github.com/tencentmalos/xrgame-native/tree/dc877ba12dfe3ad32138c38b3f8bf33e64e72142)，再按下列路径定位。本文与显示专项是该基线之后的本地文档，交接时请提供这两个文件，不假设远端已有它们。

- **D1**：[LinuxRuntime](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/runtime/LinuxRuntime.java)、[SessionService](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/session/SessionService.kt)、[droiddeck-session](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-session)。
- **D2**：[steam-compatibility](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/steam-compatibility)、[droiddeck-proton-run](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-proton-run)、[droiddeck-fex](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-fex)、[Android build](../../references/DroidDeck/app/build.gradle)。
- **D3**：[AHB swapchain](../../references/DroidDeck/app/src/main/cpp/waylandcomp/src/ahb_swapchain.c)、[SurfaceControl layers](../../references/DroidDeck/app/src/main/cpp/waylandcomp/src/sc_layer.c)、[compositor](../../references/DroidDeck/app/src/main/cpp/waylandcomp/src/compositor.c)。
- **D4**：[ProotFastPath](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/runtime/ProotFastPath.kt)、[SessionPrefs](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/session/SessionPrefs.kt)、[fastpath.c](../../references/DroidDeck/tools/proot/fastpath/fastpath.c)、[上游分析方法](../../references/DroidDeck/docs/development/proot-performance.md)。
- **D5**：[FakeInputWriter](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/input/FakeInputWriter.java)、[PadBridge](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/input/PadBridge.java)。
- **D6**：[KeyboardHost](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/input/KeyboardHost.kt)、[wl_text_input](../../references/DroidDeck/app/src/main/cpp/waylandcomp/src/wl_text_input.c)。
- **D7**：[agent control](../../references/DroidDeck/docs/agent-control.md)、[AgentBridgeProvider](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/agent/AgentBridgeProvider.kt)、[SessionState](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/session/SessionState.kt)、[scenario runner](../../references/DroidDeck/tools/droiddeck-scenario)。
- **D8**：[Quest 范围](../../references/DroidDeck/docs/quest.md)。
- **D9**：[DirectAudioRelayComponent](../../references/DroidDeck/app/src/main/java/com/droiddeck/launcher/audio/DirectAudioRelayComponent.java)、[音频制品 pin](../../references/DroidDeck/tools/directaudio/release.env)、D1 的 `startAudio`、D4 的 `directAudio` 偏好。
- **D10**：[droiddeck-esync](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-esync)、[sync bundle pin](../../references/DroidDeck/tools/droiddeck-esync/release.env)、[gamescope pin](../../references/DroidDeck/tools/gamescope/release.env)。这些外部组件没有因本次 reference 被采用为我们的依赖。
- **D11**：[Steam 下载与校验脚本](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-install)。官方客户端、Proton 和 runtime 的来源需分别追踪，不能用应用版本代替。
- **S1**：[Bionic launcher](../../app/src/main/java/com/winlator/xenvironment/components/BionicProgramLauncherComponent.java)、[Steam client staging](../../app/src/main/java/app/gamenative/xrgame/XrGameSteamClient.kt)、[Cloud RPC](../../app/src/main/java/app/gamenative/xrgame/XrGameCloudRpc.kt)、[现有系统设计](../architecture/xrgame-native-system-design.md)、[picoXr build](../../app/build.gradle.kts)。系统设计早于部分 XR 工作，XR 现状以 S5 为准。
- **S2**：[PresentExtension](../../app/src/main/java/com/winlator/xserver/extensions/PresentExtension.java)、[MHW 异步呈现验证](../validation/mhw-present-async-20260928.md)、[MHR 验证](../validation/mhr-present-async-20260929.md)。
- **S3**：[evshim](../../app/src/main/cpp/evshim/evshim.c)、[IMEInputReceiver](../../app/src/main/java/app/gamenative/externaldisplay/IMEInputReceiver.kt)、[IME tests](../../app/src/test/java/app/gamenative/externaldisplay/IMEInputReceiverTest.kt)。
- **S4**：[DebugBus](../debugging/debugbus.md)、[Litep](../debugging/litep.md)、[API replay](../validation/api-replay-20261004.md)。
- **S5**：[SBS](../validation/vr-sbs-20261002.md)、[Swan composite](../validation/swan-xr-composite-20261009.md)、[SGSR 直写 swapchain](../validation/swan-upscale-direct-20261010.md)。
- **S6**：[本地 Apple Container 构建](../building/apple-container.md)、[runtime 说明](../../tools/xrgame/README.md)、[THIRD_PARTY_NOTICES](../../THIRD_PARTY_NOTICES)。
- **X1**：独立 shadPS4 工作树 `39b449360111faa213b17bc80365abd0750e1c50`：[Android Host 构建](https://github.com/tencentmalos/Bachata-S4/blob/39b449360111faa213b17bc80365abd0750e1c50/scripts/android/build-host-android)、[FEX/HLE 边界](https://github.com/tencentmalos/Bachata-S4/blob/39b449360111faa213b17bc80365abd0750e1c50/src/core/guest_cpu/fex/fex_context.cpp)、[guest payload 构建](https://github.com/tencentmalos/Bachata-S4/blob/39b449360111faa213b17bc80365abd0750e1c50/CMakeLists.txt)。只支持本文的架构边界说明，不代表 Wine 自动拥有 shadPS4 的 HLE 或调试能力。


## 2026-10-17 增量：官方客户端 UI 的借鉴边界

本文前面的“当前”指 10 月 13 日固定基线；`feature/linux_steam_support` 后来增加了客户端专用 PRoot，不能继续用“完全没有 PRoot”描述这个新分支。游戏仍直接走 Bionic Wine，详细实测见 [官方 Steam 记录](../validation/linux-steam-20261016.md)。DroidDeck reference 仍是本文固定 pin。

| 问题 | DroidDeck 的具体处理 | 新分支的处理/后续边界 |
|---|---|---|
| Android 软键盘 | `KeyboardHost.kt` 提供显式入口，焦点编辑器转按键；不是 Steam 文本框自动探测 | 全屏页补回键盘按钮；复用现有 IME，客户端支持 CJK composition 和 UTF-8 文本提交。不能照搬其密码字段类型，否则 Gboard 拼音被限制为 Latin |
| UTF-8 剪贴板 | `SessionClipboard.kt` 限前台监听；`tools/linuxfs/clipboard/clipboard.c` 提供 X11 TARGETS/UTF8_STRING/TEXT，STRING 仅 ASCII | 只给 IME 当前提交使用临时 selection，文本走 stdin，无系统剪贴板监听/共享文件；不更改游戏输入模块 |
| 中文字体 | `droiddeck-fonts` 修复 Fontconfig 默认配置，session 绑定 Android 字体并传 CJK locale | Ubuntu Fontconfig/Noto 已能匹配中文；另补 Xephyr core font catalogue，不能混淆两条字体通路 |
| 大屏刷新 | session 大屏参数为 `-cef-force-gpu -cef-use-gl=angle -cef-use-angle=vulkan`；KDE 桌面才 `-cef-disable-gpu`。Wayland compositor 按 Choreographer tick 合并 dirty 更新 | 当前 CEF/Xephyr 软件路径约 15 Hz 的基线不能冒充 GPU 瓶颈结论；硬件 CEF 还需打通 glibc GPU/WSI，不是删除一个 flag 就完成。客户端优化应与现有游戏 AHB 路径分开验收 |
| 现有下载复用 | `droiddeck-steam-games` 使用实际 `.DepotDownloader/depot.config` installedManifestIDs，生成/导入 ACF，并链接游戏目录；`steam-compatibility` 注册自有启动工具 | 10 月 17 日已接入实际 depot ACF、唯一 steamapps、Official/Custom 写入互斥及既有 prefix/RemoteStorage。AYN 官方识别原 7 项，并将公共运行库下载到同一实体目录，Custom 随即读到；实际游戏启动与读档另验，见[共享库记录](../validation/steam-shared-library-20261017.md) |
| 外部存储文件锁 | `tools/linuxfs/preload/lock.c` 先尝试真实 flock/fcntl，仅对文件系统不支持锁的 errno 降级 | AYN 的 FUSE flock 返回 ENOSYS，导致 Steam 打开 ACF 后仍报加载 0 项。已将 pinned 源文件编译进客户端专用 shim，修复后识别及下载通过；不是提供了 FUSE 跨进程锁，仍须禁止两种模式并发改库 |

认证仍保留独立登录：未找到官方受支持的桌面会话导入接口，不读取/复制认证文件。Bionic 客户端能连接官方会话已测，真实游戏 Steamworks 与联机仍待验。
