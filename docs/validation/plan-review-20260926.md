# xrgame-native 规划回捞与复核（2026-09-26）

**后续规划已更新：** 用户确认个人学习优先、AYN 验证、Swan 延后，并将原 WP3 收尾
移为最后的可选项。当前入口为 [个人学习版 v2](../specs/xrgame-native-personal-v2.md)；
下文保留当时对旧规格的复核，不再作为当前执行顺序。

本记录复核当前需求、依赖关系与实施证据，不替代 v1.2 规格，也不把建议当作已批准的需求变更。复核基线为 `feature/malos/wp1-steam-install` @ `ca045ed33132de7582a87cc51c3eb8f4ed94b84c`，加本机未提交的两个测试修复、主机说明与 WP1 续验记录。此次没有新增设备测试、运行构建或更新 references pin。

**后续进展提示（同日）**：下文保留规划复核时的历史基线。之后已在 AYANEO 上用内置 Proton 11/FEX 跑通真实 x64 7-Zip，并补运行时构建入口、源码归档、下载锁和发布前检查；最新状态以 [WP3 实施记录](wp3-build-pipeline-20260926.md) 为准，不能继续把下表的早期测试失败数作为当前结果。

## 1. 哪份规划有效

| 文档 | 作用与当前边界 |
|---|---|
| [2026-09-10 初始路线](../background/android-x64-arm64ec-plan.md) | 历史背景。其“bionic Wine × ARM64EC 尚无公开组合”“必须 ARM64 构建机”以及 P0–P3 已被后续重评估取代，不再用于安排本项目。 |
| [2026-09-24 路线重评估](../background/android-windows-games-reassessment-20260924.md) | 解释为什么消费社区底座、哪些 shadPS4 资产可复用。它以重写宿主为中心的 R0–R5、周数估计、32 位覆盖不是本项目当前承诺。 |
| [xrgame-native v1.2](../specs/xrgame-native-v1.md) | 当前需求与验收入口。采用 GameNative fork，先 Steam 安装，再独立运行时与 Pico 适配；重写显示栈放到 WP7 测量后的 v2 决策。 |
| [WP0 记录](wp0-picoxr-identity-20260924.md)、[WP1 记录](wp1-steam-install-20260924.md) | 实施证据；以记录中的具体构建、设备与测试结果判断完成程度，不能用规格文字替代实测。 |

路线仍适合当前目标：在独立包名下复用 SteamService/JavaSteam、bionic Wine + ARM64EC/FEX、现有显示和 XR 桥接，优先打通 Pico 上的影院及 PCVR。当前资料不足以证明这组组件在 Swan 上的兼容性或性能最优；规格 §1.1 的“结构上最优”应理解为选型判断，不能当作测量结论。Windows OpenXR runtime 本身仍是 x64/x86 PE（规格 §2.2），也不能笼统声称只有游戏代码需要翻译。

## 2. 当前做到哪里

| 工作包 | 当前状态 | 下一道验收门 |
|---|---|---|
| WP0 仓库与独立身份 | 已完成并合入 `malos/main`；首次上游同步演练无新增差异，尚未证明真实冲突处理能力 | 持续维护身份、CI 与同步记录 |
| WP1 Steam 安装 | 源码构建与安装路径改造已有；设备验收部分完成，CI 未通过 | 下载、续传、文件校验、PC 对照/导入、重装识别与实际出网证据 |
| WP2 去服务依赖与许可清理 | 只有部分前置处理及待办，未完成 | 关闭功能入口、补来源声明、APK 审计 |
| WP3 自建组件与 manifest | JavaSteam/libgndownload 已先行，完整运行时管线未完成 | 可追溯的基础运行时、组件索引、安装校验及组合运行验证 |
| WP4 独立 shim | 未完成 | Swan 上行为矩阵与最小 Windows 程序链路 |
| WP5 Pico XR | 尚未适配验收；当前 `picoXr` 使用非 XR 源集 | 能力探测、影院、OpenXR、OpenVR 及生命周期验收 |
| WP6 Steam 启动与云存档 | 上游代码可供复用，未在本发行版闭环验收 | 首次运行依赖、游戏启动、存档上传及恢复 |
| WP7 性能基线 | 未完成 | Swan 同窗 CPU/GPU、帧时间、内存数据，再决定 v2 |

已确认的现实起点：

- Thor 已装 XRGame Native，Steam 标签可见 188 个条目，`7 Billion Humans` 目录已存在。它们不能证明库完整、文件校验或游戏可运行。设备 APK 是早期 WP1 构建 `f3495a58…`，不是最新分支构建，详见 WP1 §5.2。
- 按用户 2026-09-26 的意见，从现有 Steam 库选取测试游戏，不再把“用户另行给游戏清单”作为继续工作的前置条件。`7 Billion Humans` 是小于 2 GB 用例的候选，仍须确认 Windows depot、完整安装及适用性；大游戏从已有库中选择。PC 对照副本可用同一测试授权经 DepotDownloader 建立，PC 导入用例仍须保留独立证据。
- 先 Thor、后 Swan 的顺序有效；Thor Android 13 上通过，不替代 Swan Android 16 的 WP1 复测、W^X 和 XR 验收。
- 最新已查询 Linux CI `35988070277` 在测试步骤超时。本机修复后全量不再挂起，但 1364 个测试仍有 5 失败、7 跳过；不能标记 CI 或 WP1 通过。详见 WP1 §3.2–3.3。
- 当前 Steam 功能是集成的协议客户端；v1 的运行路线仍按 WP6 的模拟模式。看到 Steam 库，不等于已安装并验证 Windows Steam GUI 客户端。

## 3. 需要修正的规划问题

### R1 — 高优先级：工作包出口有倒置依赖

规格 §5 写 WP4 依赖 WP3、WP5 依赖 WP4，但 WP3 的完整原生库清单包含“WP4 的 shim”（第 344 行），WP4 的 D3D11 出口又要求“在影院模式下出画面”（第 389 行），而影院验收在 WP5（第 433 行）。若逐包全部完成才开始下一包，会形成循环。

建议保留现有 WP 编号，区分阶段交付：WP3 先交付供 WP4 探测使用的基础 Proton/FEX/图形组件；WP4 先验收普通 Android 窗口中的 D3D11 测试程序，将影院出画面留给 WP5；WP3 的最终组件集合在 shim 完成后收口。若 WP4/WP5 选用的游戏需要 WP6 中的 Steam 模拟配置或 redist/installscript，则把这些最小启动前提提前准备；云存档仍在 WP6 完整验收。

### R2 — 高优先级：WP3 组件源与 WP1 全局白名单冲突

[XrGameEgress.kt](../../app/src/main/java/app/gamenative/xrgame/XrGameEgress.kt) 第 30–44 行只列 Steam/Valve 服务域名，第 68–78 行替换默认代理选择器。WP3 计划从自有 GitHub Releases/manifest 下载组件，这些地址同样会被当前机制拦截。现有 [ManifestRepository.kt](../../app/src/main/java/app/gamenative/utils/ManifestRepository.kt) 第 14 行还指向上游 master，尚未接入本仓索引。

建议明确两类访问的验收范围：Steam 登录/库/游戏下载流程只访问为 Steam 提供服务的端点；组件安装流程访问受控的自有发布索引与其登记产物。不要通过简单放开整个 GitHub 域名解决，否则同域上的上游 master 索引也会恢复可访问。组件 URL、重定向后的下载来源、版本、SHA 及失败行为一起验证，仍禁止回退上游。

### R3 — 高优先级：白名单不是全进程 Valve-only 的证明

同一源码第 24–26 行明确排除 raw socket、JavaSteam CM 和 Rust 下载引擎；第 52–65 行还允许公网 IP 字面量。`AGENTS.md` 第 9 行及 WP1 实现表中的“non-Valve egress is blocked / 白名单只含 Valve 域名”概括过强。

应将现状表述为“依赖默认 ProxySelector 的请求已有域名限制”。WP1 原定的按 UID 连接/DNS 观测仍必须执行，并覆盖登录、封面、下载、校验、失败重试等实际路径。放行公网 IP 和绕过选择器的路径需要单独盘点；不能用拦截日志代替抓包或连接证据。

### R4 — 中优先级：导入需求与当前候选实现不等价

规格 WP1-5 要求复用 `importCustomGameAsSteamGame`，并用于离线场景。WP1 §6 当前候选是原目录执行安装，联网取得 manifest/depot key 后逐 chunk 用 Steam Adler32 复用已有数据；上游 CustomGames 复制路径只按字节数检查，不能作为相同的校验保证。

保留这项差异，先在设备测“已有文件保留、缺失或不同文件补齐”，再由用户确认是否调整 WP1-5 的离线承诺。不能直接把在线复用判作离线导入已完成。DepotDownloader 同 app/depot/manifest、Windows 平台、语言的逐文件 SHA-256 对照继续保留。

### R5 — 中优先级：构建环境描述遗漏了 macOS 路径

规格 §2.2/§6 将 XR 构建描述为 Windows-only，但仓内已有 [build-xr-payload-macos.sh](../../tools/build-xr-payload-macos.sh)：第 40 行起编译 PE runtime，第 53 行起编译 `libxrimmersive`，第 69 行起编译 unixlib；`--bridge` 经 Linux amd64 Docker 构建 ARM64X builtin。Gradle 的 XR 自动任务仍调用 PowerShell，所以“Gradle 集成依赖 Windows”与“全部 XR 组件只能 Windows 构建”要分开。

当前主机是 macOS ARM64。WP3 应先核验和复用已有脚本，并将产物接到 `picoXr` 的构建输出，而不是重复建设。现脚本写入 `modernXr/legacyXr` 源目录、会使用部分预编译件，也没有在此次复核中执行，不能宣称已满足完整源码构建或 Pico 打包验收。

### R6 — 中优先级：可复现与组件兼容性需要更精确的出口

WP1 已记录同源码跨 Windows/Linux 的 `libgndownload` Build ID 不同、JavaSteam jar 跨主机 SHA 不同。现有证据支持固定源码、构建配方及产物身份可追溯，不支持跨主机逐位一致。WP3 应明确前者为当前出口；如果要逐位可复现，另列固定工具链、路径/时间戳与字节对比要求。

另外，规格 WP3 使用的 proton-wine pin、XR bridge 构建引用的 Wine revision、FEX pin、DXVK/VKD3D 来源并非单一已验收组合。先记录一个完整版本集合，并验证 prefix、ARM64EC/FEX 加载、D3D11、XR bridge ABI，再扩大构建管线。单个组件编译成功不等于组合兼容。

### R7 — 中优先级：提前执行已有测量门，补充最小生命周期检查

WP5.1 本来允许提前，建议在 WP1 收口期间准备，并在获得 Swan 操作授权后实测 loader、GLES binding、扩展、控制器。若必需的 GLES binding 不满足，规格 §7 已注明需另立 Vulkan 适配；应尽早获知。

WP4 已要求 Stop 无残留，WP5 已要求连续 10 分钟及摘戴/前后台恢复。建议在最小图形样例可用后补重复启动/退出与会话边界检查，防止原有“杀同 UID 其他进程”的清理逻辑影响其他服务。内存占用可随各次冒烟采集；WP7 再做正式同窗归因，不必等 WP7 才发现无法维持会话。

### R8 — 低优先级：维护文档与源码事实对齐

- `references/README.md:3` 和 `AGENTS.md:20` 仍说 references 从不构建进 app，与已实施的 JavaSteam 源码构建矛盾，应区分“只读参考”与“固定版本构建输入”。
- 规格 §6/§9 的“游戏清单待用户告知”已被本次会话“从现有 Steam 库下载”的操作方向补充；主机和 Thor Android 版本以当前实测为准，不沿用背景方案的 Windows/Android 14 假设。
- WP2 的预编译豁免不应被理解为可豁免 C2 点名禁止的包名锁定/闭源运行路径；未知来源项先登记，不等于通过最终验收。
- 构建、下载、导入、运行、XR 和性能各自记录通过状态，不用一个“已内置 Steam”或“APK 能打包”替代其他阶段。

## 4. 建议执行顺序

1. **收口 WP1。** 先解决剩余测试隔离问题并验证 Linux CI；从 Thor 当前 Steam 库选择小/大游戏，完成规定的续传与 SHA 对照。设备 APK 更新、重装等动作按已有授权边界执行。随后补 Swan；PC 导入和重装识别不从验收表中删除。
2. **交错推进 WP2、WP3 基础构建与 WP5.1 探测。** 清除上游服务入口、做来源清单，同时产出自有版本集合和能力探测结果。无需等全部 Steam 数据测试结束才开始代码审计与构建准备。
3. **完成 WP4 的 Swan 行为矩阵和最小平面图形链。** 先 prefix → x64 控制台 → GDI → D3D11，按所选游戏需要准备最小 Steam 启动依赖；然后验证一款 WP1 游戏主菜单。
4. **完成 WP5。** 影院 → Windows OpenXR 测试程序/游戏 → OpenComposite/OpenVR，再执行输入、触觉、摘戴、前后台与持续运行验收。
5. **完成 WP6/WP7。** 云存档闭环与正式性能采集；以数据决定 v2 是否替换显示栈。旧背景文档中的周数不用于承诺当前排期。

本次仅形成复核记录。规范变更、代码修复、组件发布和设备操作仍按各自的任务与授权推进；未 commit、push 或更改 v1.2 原文。
