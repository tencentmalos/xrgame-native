# XRGame Native 个人学习版规划 v2

状态：**用户已要求推进 P1、P2、P3；实施中，尚未验收**。2026-09-27。

2026-10-02 当前增量：[Android SBS 与 Swan VR 接线规划](xrgame-native-vr-sbs-v1.md)。先用 AYN 验证独立双眼 VR 提交和模拟追踪，Swan 实机验收仍延后。

最新逐项结果与缺口见 [2026-09-27 实施状态](../validation/personal-status-20260927.md)。

2026-09-28 新增[Mac 独立构建与预编译依赖规格](xrgame-native-macos-build-v1.md)：
**用户同日已暂停该构建迁移（ROI 低）**；保留评估，后续按需恢复。当前优先将
Present fence 等待移出 X 请求线程及全局锁，按 [MHW profiling](../validation/mhw-profiling-20260928.md)
做改造与同场景对照。该调整不改变下文 P1–P3、Swan 延后及设备验收边界。

2026-09-27 用户补充：Steam 客户端、DXVK、VKD3D 及 D3D10/11 配套 DLL 必须作为
默认随包运行时，新建与已有容器都在启动前校验并自动修复。游戏专用参数仍按需覆盖，
不能依靠逐游戏补齐必备 DLL。实测与边界见 [随包运行库记录](../validation/bundled-runtime-20260927.md)。

用户已确认：以个人学习和实际运行为先，使用 **AYN Thor** 验证，Swan 相关工作延后；
原 **WP3 放到最后，作为可选项**。本文取代 [v1.2](xrgame-native-v1.md) 的执行顺序与阶段门槛。
旧规格保留为技术背景；已有验证记录继续有效，但只证明对应设备与构建。

原始要求的逐项对照、shadPS4/FEX 的可复用进展和调试扩展见
[原 spec 与可分析性复核](../validation/spec-observability-review-20260926.md)。
本文的 AYN 阶段通过不能替代原 spec 的 Swan/XR 出口；云同步等延后项仍保留未完成状态。

## 1. 当前目标与保留约束

先在 AYN 上把 Windows / Steam 游戏的启动、显示、输入、声音、存档和退出做实，
能修改、调试和比较 Proton / FEX / Turnip；之后按需要迁移到 Swan / XR。

- 当前使用内置的固定 Proton 11 ARM64EC、FEX、DXVK/VKD3D、Turnip 和 imagefs。
  本地 catalog 的版本与 SHA 校验继续保留，不依赖公开 Release 或正式远端 manifest。
- 继续使用独立应用身份、本地构建脚本与自有 fork；主仓基于 `malos/main`。
  commit / push 仍需用户明确要求，子仓未发布前不推进父仓 gitlink。
- 不引入闭源 redirect、包名锁定组件、Steamless 或上游 GameNative 服务；
  自研 shim 只按实际问题做 clean-room 实现。
- 记录所用源码版本、补丁、APK / 组件身份与失败证据；保留已有许可声明。
  完整发行源码归档、远端 CI 和公开发布不作为当前功能工作的门槛。
- Steam 使用现有专用测试账号，由用户输入凭据；游戏数据、账号、APK 和密钥不进仓库。

当前 AYN 实测身份：Thor、Android 13 / API 33、4 KiB；已装 XRGame Native 的
targetSdk 为 36。**targetSdk 36 不等于 Android 16**，此设备的行为不能替代 Swan 结论。
设备序列号、完整系统身份与 boot ID 仍只存于仓库外证据目录。

## 2. 已有起点

| 已有能力 | 证据与边界 |
|---|---|
| 原生库、Proton/FEX/D3D/Turnip 本地源码构建，内置运行时与 SHA 校验 | 已完成构建及内部打包；新容器重放通过，不宣称位级一致或远端 CI 通过 |
| Windows x64 控制与窗口链路 | AYANEO 上 7-Zip 可浏览、创建目录、正常退出；尚需在当前 AYN 复测 |
| Hades II D3D12 游戏路径 | AYANEO 上已进入场景、键盘移动、退出重启；不是当前 AYN 的通过记录 |
| AHB 显示 | Guest AHB → Host GPU 拷贝 → 显示；无 CPU 读回，present-wait 开启；尚有一次中间 GPU 拷贝 |
| 调试 | Android native 与 Wine x64 guest 的 attach / inspect / pause / resume / cleanup 已跑通 |
| Steam 下载与导入 | 有实现与部分设备证据；完整续传、校验、导入、对照仍未验收 |

详见 [运行时构建](../validation/wp3-build-pipeline-20260926.md)、
[Hades II](../validation/wp3-hades2-20260926.md)、
[调试链路](../validation/wp3-debuggers-20260926.md) 和
[Steam 安装](../validation/wp1-steam-install-20260924.md)。

## 3. 新的执行顺序

P 编号仅用于本版规划；旧 WP 编号保留在历史证据中。

| 顺序 | 阶段 | 对应旧工作包 | 当前安排 |
|---|---|---|---|
| **P1** | **AYN 运行基线与执行兼容** | WP4 + 已有运行时 | **建议从这里开始** |
| P2 | Steam 日常使用闭环 | WP1 / WP6 | P1 后补齐；遇到影响启动的问题可提前处理 |
| P3 | 显示同步、减少拷贝与性能比较 | WP7 的平面部分 | 基础运行稳定后进行 |
| P4 | Swan / XR | WP5、WP4 的 Android 16 复验、WP7 的 XR 部分 | **延后，用户要求时再开始** |
| **P5** | **原 WP3：工程化与发行收尾** | **WP3** | **最后，可选，不阻塞 P1–P4** |

可分析性随功能推进：P1 先补 D0 进程身份/清理和 D1 符号/双栈；P2 继续 D1 并逐项验证
D2 guest 执行控制；P3 改显示前先补 D3 帧/缓冲区/等待链。D4 ARM64EC 函数探针依据热点选择。
这些是 2026-09-26 的新增细化，不是声称原 spec 已要求或 XRGame 已实现完整 debugger。

### P1 — AYN 运行基线与执行兼容（先做）

先复用已跑通的运行时，查清当前设备确实需要什么兼容处理；不预设必须编写 shim。

1. 核对 AYN 已装 APK、运行时版本及游戏安装状态；必要时保留数据更新到已验证版本。
   从现有 Steam 库选择已安装、可校验的游戏，Hades II 若未安装则通过 app 安装，
   或按现有导入流程提供副本。无需等待全部 Steam 安装测试完成。
2. 在实际 app / Wine 运行域记录行为矩阵：
   - app 私有目录 ELF 直接 `execve` 与 `/system/bin/linker64` 启动的结果、errno；
   - argv（空参数、空格、非 ASCII）、envp 与子进程退出状态；
   - 文件 `mmap(PROT_EXEC)`、匿名内存 RW→RX / RWX 的结果；
   - `/proc/self/exe` 在上述启动方式下的值及 Wine 是否依赖它；
   - evdev / hidraw 访问实际引发的问题。只观察必要行为，不先全局屏蔽。
   记录 UID、SELinux 域/模式、系统版本和 targetSdk，不能用 root / adb shell 下成功代替 app 域结果。
3. 仅修复矩阵与真实运行暴露的问题。路径问题优先构建期修正；确需 exec 转发、
   `/proc/self/exe` 修正或映射回退时，再加入最小实现。重写/决策函数做主机单测，日志默认关闭。
   如果不需要额外 shim，就记录“当前 AYN 组合无需新增 shim”，不为完成工作包而强行添加。
4. 复测 prefix → x64 控制台 → GDI 窗口 → DXVK D3D11 样例 → 一款 Steam 游戏。
   Hades II 是 D3D12 用例，不能替代独立的 D3D11 检查。

**本阶段验收目标：**

- 新测试容器完成 prefix 初始化，不清除已有账号、容器与游戏数据。
- x64 控制台输出/退出码正确；GDI 窗口可操作；D3D11 样例在普通 Android 窗口显示。
- 一款游戏进入实际场景；分别检查键鼠、物理手柄和音频，明确未通过项。
- 游戏连续运行至少 10 分钟；完成 3 次启动/退出，Stop 后无该会话的 Wine 残留。
- 行为矩阵、APK/组件身份、进程与时长归档在 `docs/validation/` 对应记录，原始证据放仓库外。

这不是 Swan / XR 验收；影院模式不再是 P1 的前置条件。

### P2 — Steam 日常使用闭环

- 下载、暂停/中断后续传、文件校验、已有目录导入与重装后识别；优先使用现有库内游戏。
- 至少一款游戏与相同 app / depot / manifest 的 DepotDownloader 副本对照文件 SHA。
- 把当前 Hades II 的手动启动配置整理为可追溯的应用流程；补首次运行依赖和明确错误提示。
  GBE loader 已从固定源码在 Linux 构建并进入私有验证；偶发启动退出仍在定位，完整发行来源材料留在 P5。
- 验证本地存档、退出后重启读取与应用更新后的保留；云同步单列可选验证，不阻塞本地使用。
- 对实际登录、下载和启动流程核查上游服务请求，不能只凭代理白名单认定全流程已隔离。

**出口：** 一款现有 Steam 游戏可从安装/导入，经过启动与操作，到本地存档和重启恢复；
下载/导入/对照各项分别记录，不用“已内置 Steam”代替结果。

### P3 — 显示同步与减少拷贝

当前 AHB GPU 拷贝路径作为可运行对照。先测 CPU/GPU、帧时间、内存与同步等待，
再让 Host 直接采样 Guest AHB，尝试省去中间 `vkCmdCopyImage`。

- Guest 完成写入后 Host 才能读取；Host 最后一次 GPU 读取结束后，才允许 Guest 复用。
- Present Complete / Idle、缓冲区引用、窗口销毁、分辨率变化和退出重启须一起验证。
- 在同版本、同场景和相同设备配置下比较拷贝与直接采样路径；不把 app 叠加层 FPS 当作唯一依据。
- 先优化呈现与同步；是否把 X server 核心、音频等改为其他 Host 实现，由测量决定。
  X server 并非 x64 翻译器，替换它不是本阶段运行 Windows 游戏的前提。

**出口：** 直接采样在上述生命周期中正确，并有与现有路径的性能/稳定性对照；
若收益不足或兼容性退化，保留当前路径并记录原因。XR 最终合成另在 P4 验证。

### P4 — Swan / XR（延后）

待用户重新安排 Swan 后，按以下顺序推进：

1. 在 Android 16 上重做 P1 行为矩阵，不能沿用 AYN 的允许/拒绝结论。
2. 探测 Pico OpenXR loader、扩展、刷新率、视图与控制器能力。
3. 2D 影院模式、控制器输入、前后台与摘戴恢复。
4. Windows OpenXR 样例/游戏双眼提交；需要 OpenVR 时再接入 OpenComposite。
5. XR 同窗性能采集，再决定进一步替换 Host 组件的范围。

OpenComposite 是选择 OpenVR 功能后的依赖，不要求为当前平面 Windows 游戏预先完成。

### P5 — 原 WP3：工程化与发行收尾（最后，可选）

**原 WP3 不再作为 WP4 / P1 或 Swan / XR 工作的前置门槛。**
已经完成的本地源码构建、内置运行时、哈希校验和调试能力继续使用；不标成未做，也不撤销。

按以后实际需要选择：

- GitHub Actions 的完整构建/测试与自动化打包；
- 发布所需的完整对应源码归档、依赖来源证明与发行许可材料；
- 子仓变更发布、组件 GitHub Releases、正式远端 manifest / 更新机制；
- 更严格的跨机器重建、构建差异归因与位级可复现。

未选择本阶段时，使用本地构建和内置 catalog，不需要公开组件服务。
如果以后决定公开分发，再完成对应的来源、许可与发布检查；现有发布门禁不改为绕过。

## 4. 当前实施起点

**从 P1 的 AYN 基线复测开始：核对并更新已有包 → 跑 x64 控制台/7-Zip →
检查一款已安装游戏 → 收集 app 域 exec/mmap/mprotect 行为矩阵。**

AYN 已保留数据更新至带 app 域行为探针的 AHB5 内部包，Steam 库与已有游戏仍可见。
exec/mmap/mprotect 矩阵、x64 控制台/GDI/D3D11 样例已取得结果；Hades II 已通过
应用内启动进入实际场景，完成三次干净退出，并重载本地存档；一次保留数据更新前后的存档哈希一致。
Host/Guest 调试连接与
清理已有证据，Guest 字节序已纠正；FEX 回调保护已修复软件断点恢复，基础单步及 CALL/RET 通过，旧停止世代请求被拒绝；断点功能继续默认关闭。
AHB 直接采样实验已在 AYN 显示 D3D11 三角形和 Hades II 场景，已通过缩放、隐藏/恢复和退出的生命周期用例，并完成一次同场景对照；未证明可重复的性能收益，默认仍为 GPU 拷贝。
D3D11 纯清屏黑屏已通过 DXVK 延迟清屏提交修复，并在两条显示路径上复测通过。续测见
[显示与调试记录](../validation/personal-p3-debuggers-20260927.md)。详见
[P1/P2 AYN 记录](../validation/personal-p1-p2-ayn-20260926.md)。Steam 小游戏下载/校验、
大目录准备修复、强制中断续传、3050 项全文件校验、损坏修复与目录重新识别见 [P2 下载续验](../validation/personal-p2-downloads-20260927.md)，
不据此提前标记全部验收。
不为等待可选的原 WP3 收尾停住功能工作，也不提前连接或测试 Swan。

## 2026-09-27：Foundation DebugBus 接入范围

用户授权直接以私有 `tencentmalos/foundation` 子仓复用 DebugBus。P3 可分析性首先接通
Android host 的 dumpsys 命令表：状态、进程身份、运行时目录、宿主 ELF 和 Present 队列，
可开启有界呈现日志。该授权替代原先 Foundation 禁入规则，只覆盖内部研究包；
FEX/Wine/DXVK 独立进程的 provider、guest 调试控制与 GPU 细节查询不能据此标成已完成。
接入合同见 [DebugBus](../debugging/debugbus.md)，实测见 [验收记录](../validation/debugbus-20260927.md)。
