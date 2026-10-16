# Swan：guest debugger（WineDbg）与 host debugger（lldb）在 SteamPSP 上的修复（2026-10-15，10-16 整链测试）

背景：用户决定 Alyx 的 FDM/ETFR 改走 guest patch，先要求确认 guest debugger、host debugger 和 reverse study 在当前 SteamPSP 上能正常工作，随后要求“两个 debugger 顺序修”。reverse study（IDA MCP）可用，本记录只写两个 debugger。

## 环境与证据

| 项 | 值 |
|---|---|
| 设备 | Swan（Pico B3110），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，SELinux Enforcing |
| boot id | `6d4335a5-667c-426d-815b-1a82edaa0fbb` |
| APK（gdbg6，picoXrDebug） | `a96ed239aed310cf2a3f39bae58f14d5784396ea9f0ac80259c2daa669cabe88`，`tools/audit-apk` 132 个文件 0 错误 |
| `.so` Build ID | `libxrimmersive.so` `c8a0eb8afc0dbf73c0a1a5b27f06500d28ab91a8`；`libwinlator.so` `55fd2c184d32acd5c8114caddc48ebf15521db8b` |
| 组件清单（bundle manifest = catalog） | `0ca5398c69f667ca84882e873da744cbf4e884f096a4c062fce431527e3c357b` |
| Proton | `proton-11.0-2-arm64ec-xrg7.wcp` `a0de997e03ac0c07caaf94042838b61618fd2562c6f484a16cfec196e6568a38`（xrg6 `2c421539…` + `lib/wine/aarch64-unix/ntdll.so` `e305a360dc2c31abab2fd73fb03870e20cc9b37eeaad1cd7ed168084c420f8cb`；ntdll.so 无 GNU Build ID，以 SHA 记录） |
| WineDbg r7 | `0e0f48d89f98cfeca83e02c33f5ece17658140502432d20b2e11c94bf4121f20`，补丁 `winedbg-local-guest.patch`（LF）`04a9b30627448cc995da6742de210403c524926142db38ac88461fb19e507056` |
| Wine 补丁 | `wine-new-thread-sigmask.patch`（LF）`04832052dca9cfb83390fdfcc516d9fbeb7d47c70b2d9a04837d514cc0497c5a` |
| 游戏 | Half-Life: Alyx（546560），`-condebug -vulkan`，`launch 546560 load=s0/autosave` 读档进 `a1_intro_world` 后测试 |

私有证据目录 `guest-debugger-20261015/`：各版 WineDbg 与构建 JSON、每轮 helper 输出与 `journal.jsonl`、RSP 探针日志、FEX 重链接 map。失败轮次全部保留。

## Guest debugger

### 1. 启动域（SELinux）

经 `run-as` 启动的 WineDbg 是 `runas_app`，游戏和 wineserver 是 `untrusted_app`。wineserver 用 SCM_RIGHTS 传来的 fd 跨域被丢弃，WineDbg 收包失败（`STATUS_TOO_MANY_OPENED_FILES`），第一个 RSP 包就退出。

修复：DebugBus 新增 `guest_debugger`，由 app 进程启动 WineDbg。环境取自游戏进程本身：从 app 域只能走游戏同样的 no-exec 启动路径（`WINELOADERNOEXEC`、`LD_PRELOAD` 里的 exec shim）。另外去掉继承来的 `WINESERVERSOCKET`，loader 取 `PATH` 中第一个含 `wine` 的目录。`run-guest-debugger.py` 默认改用这个启动方式。

### 2. WineDbg 补丁（r7）

- `gdb_remote` 改为阻塞读，连接断开时分离且不结束被调试进程；`check_for_interrupt` 保留零超时 `select`。
- `WINE_GDB_FEX_EXCEPTIONS` 下放行除断点、单步（含 WX86 两种）以外的全部首次异常。原来只放行访问违例和两种 brk，Alyx 持续产生的 `0x80000002`（FEX 处理非对齐原子操作）会变成停止，再以 DBG_CONTINUE 吞掉。
- `+winedbg` 跟踪里记录异常地址和参数。
- r4–r6 的补丁文件在生成时用了 `-c core.autocrlf=false`，变成了约 328 KB 的整文件 CRLF 重写。二进制不受影响，r7 恢复为 26 KB。r6 曾试过让 break-in 线程从 FFS 后面的 ARM64EC 原生入口启动，结果并不能绕开 FEX，已删除。

### 3. Alyx 在附加时退出：Wine 的信号屏蔽字 bug

修复前，8 次带 break-in 的附加中死了 5 次。r3 是首次附加；r4 首次成功，同一进程再次附加时死亡；压力循环第 2 轮（PID 27178）、r5 第 2 轮（PID 28319）、r6 第 1 轮（PID 29891）也都死了。WineDbg 显示 `exit process (1)`，`console.log` 没有任何引擎关闭记录。

证据链：

- Pico 的 KillMonitor 没有任何进程发 SIGTRAP；init 报告游戏 “received signal 5”，即进程死于同步 SIGTRAP。随后 wineserver 向全部 Wine 进程发 SIGQUIT 关闭会话；app 先看到 `xr transport: producer disconnected`。
- `+winedbg` 跟踪（r5）：break-in 线程启动后只有一个首次访问违例，位于 `libarm64ecfex.dll`+0x169E4，读地址每轮相同。用构建目录的目标文件重链接出 map（只有文件头 38 字节不同），该地址对应 `FEXCore::LookupCache::FindBlock` 读 L1 缓存。这是过量提交页的正常缺页，由 FEX 的 `ResetToConsistentState` 提交。异常放行后这个线程再无事件，进程随即死亡。
- 不建 break-in 线程、只附加 15 秒的对照（测试专用二进制，不入仓）：7 轮全部存活，每轮 200 多个 FEX 异常事件。游戏自己在这期间新建了 36 个线程，同样先碰到这个 L1 缺页，全都正常。
- 原因（proton-wine `5d0d333e`）：`NtCreateThreadEx` 只额外屏蔽 `server_block_set`，`pthread_create` 让新线程继承创建者的整个屏蔽字；`signal_init_thread` 只解除 `server_block_set`。`CreateRemoteThread` 以 `APC_CREATE_THREAD` 交给目标进程里某个正在等服务器回复的线程执行。挂了调试器时，这样的线程常常正处在 `trap_handler` 或 `segv_handler` 里的 `send_debug_event` 中，SIGTRAP（或 SIGSEGV）处于屏蔽状态。新线程继承了这个屏蔽，第一次 `brk` 时内核直接以 SIGTRAP 杀死整个进程。FEX 的协作式挂起陷阱 `brk #0xCAFE` 在附加期间大量变成调试事件，使这种情况很常见。

修复（原生，Wine）：`tools/xrgame/patches/wine-new-thread-sigmask.patch`，`start_thread` 开头把屏蔽字设为 `server_block_set`。普通线程的创建者屏蔽字为空，结果不变。

- 构建：在构建机的 Wine 构建树上打补丁，只重编了 `thread.o` 并重新链接 `ntdll.so`。xrg6 包里的 ntdll.so 与该树改动前的 stage 产物逐字节一致（strip 后 `3dea4982…`）。
- 打包：`package-proton-validation.py --release xrg7`，以 xrg6 为底只替换 ntdll.so。打包前确认构建源码已含补丁（`git apply --reverse --check`），且 ntdll.so 比 thread.c 新。
- 接入：`build-wine.sh` 也会应用这个补丁。

### 4. 结果（APK gdbg6 + Proton xrg7 + WineDbg r7，Alyx PID 7717）

- 设备上 Proton 的 `ntdll.so` 为 `e305a360…`。
- 压力循环 8 轮：附加，停在 break-in 线程（`T05`），`?`、`qfThreadInfo` 后 `D` 分离。8 轮全部存活，WineDbg 均以 0 退出，每轮约 35 s。按修复前 5/8 的死亡率，连续 8 轮无死亡的概率约为 0.04%。
- MCP 完整流程：`guest_debug_start_session` → pause（epoch 1）→ continue → pause（epoch 2）→ `guest_debug_stop_session`。WineDbg 以 0 退出，adb forward 已移除，Alyx `inScene=true`、未暂停，`console.log` 继续增长。
  - 更早在 r4 上（PID 25289）读过寄存器，列出 42 个线程和含全部游戏 DLL 的模块表（`qXfer:libraries`）。
- 同一游戏进程先后被附加 10 次，没有死亡。

### 5. DebugBus stop

Android 的 `Process.destroyForcibly()` 没有覆写，只发 SIGTERM，WineDbg 不理会，所以 `stop` 原来从未结束过 WineDbg（第 2 轮起报 `debugger_running`）。现在按可执行文件名找 app 的子进程发 SIGKILL。验证：`stop` 后 `exitCode=137`，游戏继续在场景内运行。

## Host debugger（native-debugger MCP，lldb）

问题都在调试工具一侧（私有 `my_mcp_tools`），不在 SteamPSP 里。

- **tcp_gdbserver：信号策略不生效。** 读回的策略是 SIGBUS 不停、放行，但每次 ART CMC GC 的 SIGBUS 和信号 35（bionic 的 debuggerd 抓栈请求）都会让 app 停下。
  - 用 CodeLLDB 自带的 lldb 21.1.7 加 SB API 脚本复现：直接 `gdb-remote` 时，进程拿到的是主机的通用信号表，7 号是 `SIGEMT`，32–40 号没有名字。按名字设置 `SIGBUS` 改到的是 10 号（Linux 的 SIGUSR1）。
  - 连接前先 `platform select remote-android`（只选不连）后，SIGBUS 为 7，附加停止原因正确显示为 SIGSTOP。15 s 内 0 次意外停止，暂停和分离都正常。
  - 修复在 `my_mcp_tools` 分支 `codex/android-gdbremote-signals`（基于 `e794370`，未提交）：gdb-remote 附加参数增加 `initCommands: ["platform select remote-android"]`。单测通过。另有一条既有测试 `CreateAbstractPlatform_CodeLldbUsesAgdeStyleEmptyTargetAttach` 在 Windows 上失败（`/symbols` 被 `GetFullPath` 改成 `C:\symbols`），与此修复无关。
  - 已出本地包 `native_debugger_mcp 0.3.0-local.20261015.gdbsig1` 并安装，MCP 配置已指向它；旧版本保留。
  - **端到端验证（2026-10-16，新会话，MCP 进程为 gdbsig1）：** 目标是 app 进程（PID 23400，无游戏，同一 boot id）。
    - `tcp_gdbserver` 附加后，信号策略读回 SIGBUS 为 7 号，不停、放行。
    - 运行 45 s，没有任何意外停止。
    - pause 停在 epoch 3（SIGSTOP），列出 42 个线程。
    - continue 后 15 s 没有停止。
    - `stop_session` 返回 cleaned，`TracerPid: 0`，lldb-server 和 adb forward 已清理。
- **停在信号上时不能分离**（`pending_exception_disposition_unproved`）：这是工具有意的安全规则。信号表修正后，ART 的信号不再停下，正常暂停可以分离。失败的那次由手工恢复：结束 lldb-server，再对 app 发 SIGCONT。
- **abstract_platform（默认传输）：未解决。** 设备端附加成功，模块信息已返回（`jModulesInfo` 84 KB），但 CodeLLDB 随后断开（`adapter_attach_failed`），没有留下崩溃记录。用命令行手动起 platform 服务复现未成功（“unable to launch a GDB server”），暂停在这里。在此之前用 tcp_gdbserver。
- **事故（保留记录）：** 一次 lldb 命令行实验用了批处理模式。`process continue` 之后批处理不再执行后续命令，lldb 退出时由 lldb-server 对 app 发了 SIGKILL（KillMonitor：`xrg-lldb-server send signal 9`），SteamPSP 与其中运行的 Alyx 一起结束。那一局只是读档待机，没有推进进度；退出时的云同步没有执行。之后的实验都改用 SB API 脚本，在 `finally` 中分离。

## 2026-10-16：逐项处理遗留限制，整链测试

用户要求把上面的限制处理掉并测完整条链路，并把 Swan 上的 patched run-as 机制也跑通，与 DebugBus 两种 WineDbg 启动方式都保留、比较哪种更适合长链分析。

环境：同一台 Swan。经用户批准关了 verity 并重启，boot id 变为 `bf55d1cb-dcba-478b-b837-623dc3ad7d5b`，SELinux 仍为 Enforcing。APK 仍为 gdbg6 `a96ed239…`，Proton xrg7、WineDbg r7 不变。Alyx（PID 10787）读档进场景，SteamPSP 为 PID 9635。私有证据：`guest-debug-20261016/`（IDA 快照）、`host-debug-20261016/`（符号与 IDA 关联记录）、`run-as-20261016/`（run-as 原件与候选）、`guest-debugger-20261015/alyx-{appdomain,debugbus}-*`（helper 输出与 `journal.jsonl`）。

### host：XR 会话中附加，IDA 关联

- tcp_gdbserver（MCP gdbsig1）在 Alyx 场景中附加 SteamPSP：
  - 20 s 内没有意外停止。
  - 函数断点命中 `nativeWaitWindowsFrame`，栈已符号化。单步、删除断点、继续、分离均正常。
  - 期间 XR 为 90/90 Hz。
  - 符号取自 WSL 构建树中未 strip、Build ID 相同的 `libxrimmersive.so`。
- IDA 关联（`prepare_ida_live_correlation` → `ida_open_correlation` → `bind_ida_correlation` → 反编译 → `record_ida_analysis`）：
  - correlation `98060d9e0e3d4700bd7036f841fdb702`，会话 `7d615e74…` revision 3，stop epoch 3。
  - 模块 `libxrimmersive.so`，Build ID `c8a0eb8a…`，`basePrecision=verified_exact`。
  - PC `0x6D3708EB70` 对应 RVA/IDA EA `0x26bb70`，反编译为 `Java_app_gamenative_ui_screen_xr_XrNative_nativeWaitWindowsFrame`（内联了 `GetArrayLength`）。
- abstract_platform 断开的根因：CodeLLDB 1.12.1（LLDB 21.1.7）在 Windows 上经远程 platform 并行加载 app 的约 570 个模块时，自身以 exit 3 退出。
  - SB API 脚本中加 `settings set target.parallel-module-load false` 后逐个加载，约 10 s 附加成功。
  - 修复：同一 my_mcp_tools 分支把这条设置放在 abstract_platform 的 platform 命令最前，并入本地包 gdbsig3。
  - MCP 端到端验证要等重连之后。

### guest：Alyx 上的断点、单步与 IDA

- 软件断点设在 tier0 的 `Plat_FloatTime`：
  - 地址 0x6FFBAE2520，即模块基址 0x6FFBAA0000 + RVA 0x42520。RVA 取自从设备拉回的 `tier0.dll`（`fe6609ce…`）的导出表。
  - continue 后 1–2 s 内命中，rip 等于断点地址。
- 单步：
  - MCP 拒绝在自有软件断点上直接单步（`software_breakpoint_resume_unsafe`，目标没有证明支持原子 step-over）。
  - 先删除断点再单步：rip +4，rsp −0x28（`sub rsp, 28h`）。
- 删除断点后，又收到另一个线程早已排队的断点事件（rip 等于断点地址）。Wine 的调试事件按线程排队，这是正常结果；断点已撤，直接 continue 即可。
- 读栈：`guest_debug_read_stack` 需要 FEX 混合栈 provider，WineDbg 不提供（`Target does not expose the FEX mixed-stack provider`）。改用 `read_memory` 读返回地址，两轮分别是 0x6FFB8006F8（vstdlib.dll）和 0x6FF5CC307A（soundsystem.dll）。
- IDA：
  - IDA MCP 打开 `tier0.dll`（imagebase 0x180000000），存快照 `review.i64`（`b19be062…`）。
  - `guest_debug_bind_ida_database` 显式映射 0x6FFBAA0000 ↔ 0x180000000，得到 correlation `gida_6213d6c0…`。
  - 0x6FFBAE2520 对应 EA 0x180042520，反编译为 `Plat_FloatTime`（读 `QueryPerformanceCounter`，换算成秒）。
  - 模块名必须用 RSP 报告的完整名 `\??\A:\game\bin\win64\tier0.dll`。
- 停止租约：
  - 默认 30 s，到期自动 continue。断点在高频函数上时，两次工具调用之间就可能已经换成新的停止 epoch，第一轮因此出现 epoch 1→4→5。
  - 之后的会话把 `stopLeaseMs` 设为 120000。
- 每轮结束后 WineDbg 都自行分离退出（`check_for_interrupt read failed`）。`TracerPid` 为 0，Alyx 仍在场景中，游戏 46 fps，XR 90/90。

### Patched run-as 与 `--app-domain`

- 安装：用户批准后，经 MCP 环境事务（`env-f324e1a9d1e303270cca7c7c`）关 verity、重启，把 `/system/bin/run-as` 换成 MCP 内置的 `patched-run-as-37021321`（`472624b8…`）。原件 `fa1f568c…` 备份在私有证据中。
- 这一版的不足：
  - `run-as <pkg>` 仍进入 `runas_app`，和 stock 一样拿不到 wineserver 的 fd。
  - root 调用方的 `--keep-root` 留在 `su` 域。
  - 试过保留 `su` 域、只切换 UID 的 `--keep-domain`：wineserver 无法 poll `su` 域的 socket，WineDbg 报 WSA 错误 997，已放弃。
- 新增 `--app-domain`（仅限 root 调用方）：
  - 使用 package 的 UID/GID。SELinux 用 package 自己的 seinfo（不加 `:fromRunAs`），实测为 `u:r:untrusted_app:s0:c155,c256,c512,c768`，与 App 进程相同。
  - supplementary groups 仍按 run-as 的规则取（调用方的组、package 组和 shared GID），与 App 进程的组（1077、3003、9997、20155、50155）不完全相同。
  - 与内置版同源构建：`swan-w-mol` `436ae815`，system/core `c3fc9458`，`qssi_64-user`。补丁 `66a01b77…` = 原补丁 + `--app-domain`。二进制 `d96166ce…`，Build ID `2db6782d…`。
  - 目前由 root 从 `/data/local/tmp` 直接执行。系统文件要等 MCP 内置资产更新为它（gdbsig3）并重连后，再走环境事务替换。
- `run-guest-debugger.py --launcher run-as-app-domain [--run-as-path …]` 用游戏的完整环境（与 DebugBus 相同），经 `env -i` 启动 WineDbg。

### 两种 WineDbg 启动方式对比（同一 Alyx 进程）

| | DebugBus（由 app 进程启动） | run-as `--app-domain` |
|---|---|---|
| 前提 | picoXrDebug 包（DebugBusService） | adb root、关 verity、patched run-as |
| WineDbg 所在域 | `untrusted_app`，同 App | 同左 |
| helper 从请求到启动 WineDbg | 2.6 s | 3.3 s |
| 断点、单步、IDA、分离 | 通过 | 通过 |
| host lldb 停住 App 时 | DebugBus 请求 5.3 s 后超时（`IOException: Timeout`），无法启动、查询或停止 | 6.5 s 启动成功，guest 断点命中 |
| 输出 | 写到 App 数据目录的 `winedbg.log`，helper 每秒轮询；退出后保留在设备上 | 经 adb shell 直接流到主机，设备上不留 |
| 进程归属 | App 的子进程 | adb shell 的子进程，与 App 进程无关；中断时 helper 执行 `pkill -9` |
| 同时调试多个 Wine 进程 | DebugBus 只管一个 | 每个进程一个端口即可（未测） |
| release 包 | 不行（没有 DebugBus） | root 调用方跳过 debuggable 检查，理论可行（未测） |

并发测试的经过：

- host 会话 `f137a4ed…`（tcp_gdbserver）暂停 App（epoch 3），App 停了约 38 s。
- 在此期间：
  - DebugBus 请求超时；
  - 用 run-as 启动 WineDbg，guest 会话暂停后加断点，continue 后在 thread 0x1d8 命中；
  - 随后关闭 guest 会话，再恢复并分离 host 会话。
- 结束后两个进程都在，`TracerPid` 为 0，logcat 没有 ANR。

结论：

- 长链分析，也就是 guest 与 host 同时附加、host 停住 App 时还要继续操作 guest，用 run-as `--app-domain`。
- 设备没有 root 改动，或只调 guest 时，DebugBus 就够了。
- helper 的默认启动方式仍为 DebugBus。

### 工具侧

- WSL mirrored 网络会在 Windows 上保留 40524–44620 端口，即 Linux 的临时端口段。WSL 运行时，落在这一段的 adb forward 会 `cannot bind`。有一次因此留下了清理不完整的会话（cleanup 报 `listener not found`）。之后 MCP 会话改用 47790 和 47850/47851。
- MCP 待修：
  - forward 从未建成时，cleanup 应接受 `listener not found`。
  - 关 verity 重启后，`resume` 因挂载项变化报 identity changed，只能重新 plan。
- my_mcp_tools 的 Android 调试单测在 Windows 上另有 15 个环境事务用例失败。在未改动的基线上结果相同，与本次改动无关。

## 未验收

- abstract_platform 修复的 MCP 端到端验证（gdbsig3，待重连）。
- `--app-domain` 版 run-as 经环境事务替换系统文件（待重连）。release 包和多个 Wine 进程同时调试未测。
- guest 硬件断点未测；软件断点仍需 `--experimental-software-breakpoints`。WineDbg 下没有混合栈。
- 调试器附加期间 FEX 的性能影响未测。
- Alyx 的 guest patch 工作尚未开始。
