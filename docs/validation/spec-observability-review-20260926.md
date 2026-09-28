# 原始 spec 对照与 guest / host 可分析性复核

2026-09-26。基准是 [v1.2 原始规格](../specs/xrgame-native-v1.md)，不是用个人版 P1–P3
重新定义原验收。用户已要求 AYN 优先、Swan 延后、原 WP3 发行收尾最后可选，并补强调试。
下文将原要求、已验证能力、执行顺序调整和新增研究项分开记录。

## 1. 原要求逐项对照

| 原 spec | 当前证据 | 缺口 / 当前安排 |
|---|---|---|
| §1.1 / §4：bionic Proton + ARM64EC + FEX | 已内置固定 Proton 11、FEX、ARM64EC DXVK/VKD3D；AYANEO 的 x64 7-Zip、Hades II 已运行 | 路线一致。AYN 需复测；不能据此认定 Swan / XR 通过 |
| C1 / WP0：独立身份 | WP0 已合入 `malos/main`；应用身份、外部安装根已落地 | 保持；后续更新不得清除账号与游戏数据 |
| C2–C4 / WP2：去闭源依赖、上游服务和许可清洁 | 内部 APK 文件审计通过；源码构建和 picoXr 开关已有实现 | 冷启动至游戏的完整 DNS / 连接证据仍欠缺。文件审计不能替代运行时流量审计 |
| C5：构建、设备、进程和时长证据 | 已有 APK SHA、库 Build ID、catalog SHA 和私有实机证据 | 补跨进程身份、停止 epoch、调试扰动窗口、失败与清理记录；这是本次优先扩展 |
| WP1-1/2/3：JavaSteam、下载库、外部安装根 | 已实现 | 保留；有库列表和游戏目录不等于完整下载验收 |
| WP1-4/7 出口：两款大小不同游戏、杀进程续传、文件校验、DepotDownloader 比对 | 只有部分流程证据 | P2 保留全部独立检查项；必须同 app/depot/manifest 比 SHA，不能拿不同游戏版本比较 |
| WP1-5/3 出口：PC 目录导入、卸载重装后识别 | 有实现，完整验收未闭合 | P2 补验；保留现有数据。覆盖更新保留不等于卸载重装识别 |
| WP3：全组件源码/许可来源、干净重建、manifest 安装 | 本地构建、容器重放、内置包校验已有证据 | 完整发行源码闭包、远端 CI、公开组件发布列 P5 可选。不得把未发布写成已完成，也不撤销本地 SHA 校验 |
| WP4：先测 exec/W^X，再实现最小兼容 | AYANEO 实际运行说明链路可用；不是行为矩阵 | P1 补 app 域 exec/linker64、argv/envp、mmap/mprotect、proc/self/exe、evdev/hidraw。AYN 结果只能算阶段性证据 |
| WP4 出口：prefix、控制台、GDI、D3D11、游戏、Stop 无残留 | 7-Zip 与 Hades II 仅覆盖部分；Hades II 是 D3D12 | 独立 D3D11 仍要测。原 Swan / 影院出口延后至 P4，不能标原 WP4 已验收 |
| WP5.1/5.2：Pico 能力、影院、OpenXR、OpenVR | 有源码构建，不等于 Pico 实测 | 全部随 Swan 延后。OpenComposite 是选择 OpenVR 验收时的依赖 |
| WP6：GBE 模拟启动、redist/installscript、云回读 | Hades II 手工配置可启动；含固定上游 loader 的对照 | P2 先产品化启动与错误原因、验证本地存档；原云端恢复出口仍未完成。个人版云同步可选不等于原 WP6 通过 |
| WP7：同窗 CPU、GPU、帧时间、PSS/LMK，决定是否换显示栈 | AHB GPU 拷贝已解决实测黑屏；尚无完整同窗成本分解 | P3 先在 AYN 建平面基线，再比较直接采样。原 Swan 两模式各两款游戏仍属 P4 后的验收范围 |
| §1.3 / §8：替换 X server/Presenter、音频输入、FEX IR probe 为后续研究 | FEX pin 具备探针原语，XRGame ARM64EC 前端未接通 | 本轮提前可分析性；不把整套宿主重写变成 P1 前置条件，也不把直接采样等同于移除 X server |

已有证据：[WP0](wp0-picoxr-identity-20260924.md)、[WP1](wp1-steam-install-20260924.md)、
[构建](wp3-build-pipeline-20260926.md)、[Hades II](wp3-hades2-20260926.md)、
[基础调试](wp3-debuggers-20260926.md)。这些文档内的失败、中途构建和旧状态继续保留。

## 2. shadPS4 当前代码能带来什么

本次读的是实际维护的 `tencentmalos/Bachata-S4` 工作树，HEAD
`b4972cbdf8231eed2111b21b71661f4194bf54cb`。下列已核对的调试/探针源文件均与该 HEAD
一致；工作树其他未提交改动未纳入结论、也未修改。本仓 `references/shadPS4` 尚未初始化，
不能把在该目录执行 Git 后落到父仓的结果当成 shadPS4 身份。

两边 FEX 均为 `3f1f30a060b633980ed8e7674eb8d8997457edad`，工作树干净。
**同一 FEXCore 不等于同一前端能力**：shadPS4 使用 Orbis owner/HLE/VM；XRGame 使用
Wine 线程、PE 模块、ARM64EC thunk 和 Windows 异常/挂起协议。

| 已有进展（源码与记录） | 可复用内容 | XRGame 的边界 |
|---|---|---|
| `src/core/guest_cpu/debug/{target.h,rsp_server.cpp}`、`fex/fex_context.cpp`：guest pause、owner 单步、INT3、stop epoch、detach | 状态契约、拒绝陈旧状态、由执行线程提交写入、失败清理的设计与测试用例 | 不能直接挂上 Orbis RSP 控制 Wine；先完善现有 WineDbg provider，避免两个控制器争夺同一线程 |
| `ReadMixedStack`：guest leaf、有限 RBP 链、HLE/callback/wait 历史边界栈 | 每段记录来源、线程世代、invocation、限制；区分历史边界和当前 native 栈 | Windows x64 的 unwind 应优先用匹配 PE 的 `.pdata/.xdata`；ARM64EC 原生段与 thunk 另处理。不能把栈扫描当 unwind |
| `memory_watch_pass.h` + Core `DebugMemoryAccess` | 精确访存地址/宽度/指令 RIP、映射世代、明确 Unsupported | 是慢速软件观察点，不是硬件 watchpoint；原记录不覆盖 native/GPU 写入，也不覆盖全部 x86 指令 |
| `profile_pass.h` + Core `GuestProfileProbe`、`guest_auto_tag.cpp` | 实际执行指令位置、不可变 probe 表、代码身份、递归/线程归属、丢事件计数 | Windows 前端尚无安装表、生命周期、传输和帧归属。现有静态 planner 的 x64 SysV 规则不能直接充当 Windows x64 / ARM64EC ABI 证明 |
| guest auto tag 的 TMNT 实机长帧分析 | 先找实际热点，再定点加探针；区间求并、等待与 on-CPU 分开；未安装和已安装但关闭分开测 | 已证明 shadPS4 工作流可用，不是 Hades II 性能结果，也不是 XRGame 已接入 Litep |
| 2026-09-26 血源 GPU 写回破坏堆的定位 | 在写入来源记录资源/地址范围/世代，追踪迟到写回与失效 | 借鉴到 AHB buffer 生命周期和 fence/Present 归属；不移植 PS4 DRS/TextureCache 修复到 Wine |

核对路径的固定版本入口：
[guest target](https://github.com/tencentmalos/Bachata-S4/blob/b4972cbdf8231eed2111b21b71661f4194bf54cb/src/core/guest_cpu/debug/target.h)、
[FEX adapter](https://github.com/tencentmalos/Bachata-S4/blob/b4972cbdf8231eed2111b21b71661f4194bf54cb/src/core/guest_cpu/fex/fex_context.cpp)、
[探针](https://github.com/tencentmalos/Bachata-S4/blob/b4972cbdf8231eed2111b21b71661f4194bf54cb/src/core/guest_cpu/fex/profile_pass.h)、
[调试验收](https://github.com/tencentmalos/Bachata-S4/blob/b4972cbdf8231eed2111b21b71661f4194bf54cb/docs/validation/android-native-host/guest-debugger-mixed-watch-2026-09-15.md)、
[长帧验收](https://github.com/tencentmalos/Bachata-S4/blob/b4972cbdf8231eed2111b21b71661f4194bf54cb/docs/validation/android-native-host/guest-auto-tag-2026-09-16.md)。
链接用于源码身份，能力结论来自本地逐文件核对与记录，未在本轮重跑 shadPS4 测试。

## 3. 新增调试实施顺序（原 spec C5 / WP4 / WP7 / §8 的扩展）

### D0：身份、失败现场与清理，随 P1 先做

- 一个运行证据包绑定 device boot ID、APK/catalog SHA、ELF Build ID / PE SHA、UID、
  Android PID + start ticks、Wine PID、运行 ID、各 provider 的 session/stop epoch。
  Wine TID 与 Linux TID 必须有映射证据；同名线程、相同数字不能自动关联。
- Host 覆盖 app、Wine 游戏进程、wineserver 和音频进程。首先解决 native API 对
  `A:\Ship\Hades2.exe` 子进程名的识别限制，用 UID/世代/包归属绑定，不放宽成任意 PID。
- 启动前、attach 前、导出后再次验证身份；断连、Stop、崩溃和重启都写 journal。
  仅清理 owned sidecar / forward；核对目标 `TracerPid` 与恢复状态。不能仅凭 helper 返回码声称清理完成。
- 常规诊断默认关闭；只采白名单元数据，不归档环境变量全集、账号、token 或游戏内存。

**验收：** app 与 Wine 各一次 attach→pause→inspect→resume→detach；重启/PID 世代变化
拒绝旧请求；断连保留失败证据；清理后目标恢复且 owned 资源无残留。首次只读身份采集不算完整 D0。

### D1：可信的符号、双栈和异常现场，随 P1 / P2

- ELF Build ID、PE SHA/架构/加载基址、调试符号身份分别登记；ASLR 使用 module+RVA。
  分开列 x64、ARM64EC/ARM64X、Unix ELF 和 JIT code buffer。
- 优先补 WineDbg 模块枚举与 PE unwind；同时输出 native 栈，按已验证 TID/thunk 关联。
  每帧标为当前停止、异常上下文、历史边界或候选；无法展开时保留原始 PC 和原因。
- FEX 只读 guest view 输出 host PC、block RIP、mapped RIP 及精度；绑定代码区间世代与
  同构建的布局。`ReconstructThreadState` / `ResetToConsistentState` 会修改目标状态，
  不在 LLDB expression 中调用；离线解码在主机副本上进行。
  本 pin 的 `Arm64Emitter.cpp` 明确区分普通 ARM64 与 ARM64EC 的静态寄存器分配：
  例如普通 x64 JIT 的 guest RSP 在 x8，ARM64EC 路径选 x23；不能照抄旧 shadPS4
  调试文档的寄存器表。实际从匹配构建的映射/布局取值，还要核对停止阶段。
- 只透传已确认的 FEX 内部异常；保留 first/second chance、来源和原始上下文，
  不全局忽略 AV / SIGSEGV / SIGTRAP。

**验收：** 自有 x64 fixture 覆盖 guest→ARM64EC→Unix、嵌套调用、JIT fault、ASLR 重启、
过期符号负例；Hades II 等待现场能区分 guest 等待与 host 呈现等待。完整无帧指针混合 unwind
尚未实现，不以 WineDbg 原始栈读取替代。

### D2：guest 执行控制，D1 后逐项开放

先在自有 Windows x64 fixture 验证寄存器写入、未来地址断点、移除后单步、CALL/RET、
异常、代码失效和多线程；每次操作要求新的 stopped epoch。通过后再用于游戏。
Host ARM64 单步、FEX 编译回调和直接修改 guest 字节都不能独自证明 x64 执行单步/断点。
软件观察点最后接，公布支持的指令与覆盖范围；不将常开逐指令执行当作性能分析模式。

### D3：等待链与帧归属，P3 改拷贝路径前

给 Guest present、AHB 接收、GPU copy/直接采样、host submit/fence、Present Complete/Idle
关联同一 frame/buffer ID + generation。记录实际 producer、waiter、目标值与完成值，
区分无提交、提交后未完成、已完成未通知、旧帧重绘。resize/销毁/重启验证迟到事件不能释放新 buffer。
跨进程时间需声明时钟与偏差；debugger 停顿窗口排除在性能统计之外。
不在持有渲染锁时等待需要该锁才能完成的回调。

**验收：** 人工延迟 producer、正常重用、窗口销毁、分辨率变化及三次会话重启，
均能追到事件归属；无 fence 数据时报告缺失，不把等待时长写成 GPU 执行时长。

### D4：ARM64EC FEX 探针，测出热点后再接

以同 pin 的 Core 原语为基础，在 owned FEX 分支接 Windows 前端安装与回收；
为 Windows x64 调用约定、ARM64EC thunk、异常展开和返回路径增加验证。
预分配有界 ring，记录丢失/截断/递归与线程归属，默认不装探针。
只迁移公开 GPL/MIT 部分；**Foundation 代码与二进制均不得进入本仓**。

**验收：** 无探针、已装但关闭、记录开启分别比较；同场景 A/B/A，实际命中、代码变化拒绝、
递归和非正常返回可解释；guest wall time、native overlap、CPU on-CPU 与 GPU 时间分别报告。
先有已测热点再扩展，函数替换/选择性重编译作为更后的研究项。

## 4. 优化点的优先级与门槛

| 优先级 | 优化点 | 先获得的证据 / 不能跳过的边界 |
|---|---|---|
| 1 | AHB 直接采样，减少中间 GPU copy 与串行 fence wait | D3 先可追踪；guest 复用必须晚于 host 最后读取完成，保留当前 copy 回退 |
| 1 | Present/Idle 回压、无新帧时避免重复工作 | 用实际 frame ID 量化，不能只看 UI FPS；不能提前 Idle 伪造吞吐 |
| 2 | 启动阶段成本：解包/校验、prefix、redist、shader/pipeline cache | 分阶段时间和缓存身份；只在内容版本/驱动/配置兼容时复用；失败阶段明确可见 |
| 2 | VKD3D/DXVK/Wine 的锁与同步等待 | 同窗线程调度和对象 producer；先找等待来源，不盲目关闭 present-wait/ntsync |
| 2 | FEX 编译、缓存失效、重复翻译热点 | D1/D4 区分编译事件与执行热点；冷热缓存分开；缓存绑定 PE/配置/代码世代 |
| 3 | Turnip 针对 DXVK/VKD3D 的具体瓶颈 | 固定分支/构建、同场景 GPU 测量；shadPS4 PM4/DRS 优化不直接等价于 D3D 优化 |
| 条件项 | X server / 宿主合成器、PulseAudio / 输入替换 | 仍按原 WP7 的成本占比决定；先直接采样不要求重写 X 协议。FDM2 / XR 注视点随 Swan 延后 |
| 研究项 | TSO、选择性重编译、硬件观察点 | 需对应正确性/ABI证据；不得以放宽内存顺序或跳过游戏逻辑换取未经验证的帧率 |

执行上是 **P1+D0/D1 → P2+D1/D2 → P3+D3 → 按热点选择 D4**。
调试能力作为原目标的支撑提前建设；不把所有高级 debugger 功能变成运行游戏的前置门槛。

FEX [官方 crash 指南](https://wiki.fex-emu.com/index.php/Development:Debugging_Crash)
也区分 JIT block 地址与实际 guest 指令；其中旧布局、Linux 信号和 gdbserver 示例不作为
本项目 ARM64EC 的固定 ABI。实际实现以本次固定源码与 fixture 验证为准。

## 5. 本轮已落地的 D0 部分（不是完整 debugger 验收）

- 新增 `tools/xrgame/debug_identity.py`，只读检查 boot ID、同 app UID、PID/start ticks、
  退出状态与 TracerPid；重复读 boot/stat，拒绝采集中途的进程替换。
- `run-guest-debugger.py` 必须带预期 boot/UID/start ticks，启动前拒绝现有 native tracer；
  prefix/loader 解析后仍须位于所选 app；临时 staging 名带独立运行 ID。
  必须使用仓库外新证据目录，成功/失败/退出均写 journal，不记录环境值。
- 13 项定向单测通过：Wine 特殊进程名、PID 重用/重启、UID 不符、陈旧请求、退出/消失、
  路径与符号链接边界。Python 编译检查、文档本地链接及 `git diff --check` 通过。
- AYN Thor / Android 13：app PID `13526`、start ticks `121407909`，故意传入错误
  start ticks 被拒绝；未上传或启动 WineDbg，原进程身份不变且 TracerPid 为 0。
  负例 helper 的 journal 窗口为 892.007 ms，仅为检查耗时，不是游戏运行时长。
  本轮未 attach guest，也没有验证新的断连清理、Wine PID 映射、mixed-stack 或游戏性能。
- 实际安装 APK SHA 与本地一致：
  `e2616b7102f649b085136968f3f9184d307fcd607f1a1526435372b9efbe5949`；
  APK 内 catalog SHA：`83c79f72bf6ab259941d0658109c73258644fe37cd93e200a3c2f5654237d54d`。
  27 个 packaged ELF 的 Build ID/缺失标记、设备完整身份、前后快照与失败 journal
  存于私有 `personal/20260926/ayn/debug-identity-review/`；不把 packaged 列表当已加载模块清单。
  `artifacts.json` SHA：`4874370f9ccb86a4b1447d821ffacbfd90b1142261ef38d24dc5471625206b4e`。

AYN 保留数据更新已完成。更新前尝试读取 SELinux enforcing 状态被拒，初次采集提前终止，
因此没有生成旧 APK 备份；不能声称已备份或测得 enforcing 状态。已有截图与此失败说明保留。

## 6. Linux 依赖能否去掉（可行性，尚未实施迁移）

**用户随后决定暂不处理本节，继续 P1–P3 与调试工作。** 以下仅保留可行性分析，不加入当前实施队列。

需要分清设备运行环境与构建主机。原 spec §1.1 / §4 选择的已是 Android bionic Wine
+ ARM64EC FEX；§6 的 Linux x86_64 是构建 runner 要求，不是设备必须启动 Linux 发行版。

| 层次 | 当前依赖 | 可行处理与边界 |
|---|---|---|
| 设备运行 | Android/bionic、Wine PE/Unix 库；imagefs 内有 Termux 来源的 ARM64 库、配置和数据 | 已没有 Ubuntu/glibc guest rootfs 或 proot。可按 ELF 依赖、动态加载与运行测试裁剪 imagefs；不能仅按目录名删库或用 Android 系统库替代任意第三方库 |
| X11 / 音频 | app 内 X server、Wine X11 驱动、XCB、PulseAudio | 可以逐步换 Android host 实现；属于原 §8 的宿主替换研究，需要 Wine 驱动/输入/音频接口适配。AHB 直接采样本身不会去掉 X11 |
| 调试日常操作 | macOS 的 ADB、native MCP、设备 WineDbg | 不要求连接远程 Linux。当前 WineDbg 的重新编译才使用 Linux 配方 |
| APK / app native / XR payload | 现有脚本已有 Darwin 和 Linux host 分支 | 当前可在 macOS 构建；运行时包仍由已固定的构建输入提供，不等于所有组件已能本机重编 |
| FEX、Turnip、DXVK/VKD3D、WineDbg 重编 | 当前固定 Linux NDK/llvm-mingw 路径和 Wine host tools | 架构上可交叉编译；需先移植并验证 host 工具链、`widl` 等生成工具与脚本，尤其固定 bylaws ARM64EC 工具链，不能直接换任意新版 llvm-mingw |
| 完整 Proton / imagefs 重建 | Wine host tools、Android 目标库、Termux 依赖配方及 GNU 工具 | 可研究 macOS 原生构建，但现有配方只验证 Linux；工作量明显大于替换 shell 路径，尚不能承诺全部通过 |

源码核对：`prepare-linux.sh` 固定下载 Linux NDK、Ubuntu x86_64 llvm-mingw 和 Linux rustup；
`build-{wine,fex,d3d,turnip,pulseaudio}.sh` 有 host 路径假设；Wine `configure.ac` 有 Darwin 分支，
FEX 的 `toolchain_mingw.cmake` 通过 target triple 选择编译器。
[llvm-mingw 源码](https://github.com/mstorsjo/llvm-mingw)提供 macOS 构建脚本，
[NDK](https://developer.android.com/ndk/downloads)提供 macOS 工具链；这只支持“移植可行”的判断，
不证明当前固定 Proton/ARM64EC 组合已在 macOS 构建通过。

建议先把目标定为：**日常开发与调试不依赖远程 Linux 构建机**。先复用固定组件包，
再依次迁移 FEX/Turnip 等常改组件及其生成工具，最后处理完整 Proton 和依赖重建。
保留现有 Linux 配方作构建对照，不改变组件版本或父仓 gitlink。
如果要求“构建中也完全不运行 Linux”，必须走真正的 macOS 原生交叉构建；
Docker/虚拟机只是去掉远程机器依赖，仍然运行 Linux，不能算达成这个目标。
设备的 Android/Linux 内核接口仍然存在，与是否携带 Linux 发行版是两回事。
