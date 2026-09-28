# MHW 游戏内重启调查（2026-09-27）

状态：自重启后的 `Application load error 3:0000065432` 已修复，完整包已装至 AYN。
同一 Android/CCL 会话中，DX11 → DX12 → DX11 两次重启均回到 ICEBORNE 菜单，
原 3 个角色可见，正常退出返回 0，游戏/Wine 进程清理完成。此前调试轮的退出卡住、
LMK 和 framework 中断仍保留，不将一次通过扩大为长时间游玩稳定性验收。

## 已有现场

- APK：`b4861cdb71898527f689eb40657946efe8aa7aab3f12297dc9ba5814afb05423`，
  catalog：`69cf5cfa8ff065a550ab31f1b630cb84ac27418acabb033a76414a9feef5c361`。
- AYN Thor，Android 13；build
  `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；
  boot `8bb14501-5800-4b9b-a9ab-7173603812f7`。
- Build IDs：`libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`，
  `libvulkan_renderer.so` = `36421bccc5e74ef19b0ff30b47b21ea071507eab`，
  `libgndownload.so` = `674e470eca24f48e67e09f5329fc7e7a58b98469`，
  `libxrgame_debugbus.so` = `7d997790eb741cb7e6b72d1f9f2ecff5636ebf54`。
- MHW EXE SHA-256：`c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`。
- Android PID `5880` 未更换。上一轮 MHW PID `7122`，CCL 日志记录
  `WaitForSingleObject=0`、有效退出码 `1`，进程等待结束时相对 loader 日志起点为
  `350571 ms`；随后执行 registry cleanup。不能将该条和更早的 `0xc0000005` 故障混为一谈。
- 15:07:46.698 游戏窗口 unmap；15:08:03.296 Android 收到 `onGuestProgramTerminated`
  并进入 `guest_terminated` 清理；15:08:23.524 新的启动进程 `9122` 出现，游戏 PID `9219`。
  新启动非本脚本操作。旧日志没有完整的 Win32 创建进程/Steam URL 请求参数，尚不能判断
  游戏是否请求自动重新启动、由哪个接口请求，或只是要求结束后手动启动。
- 15:22:00 只读截图显示冰原雪地区域中的已装备角色；当前会话确已进入实际场景。
  这是外部操作后的观察，不是本脚本完成的受控重启、输入或性能验收。

## 最初待核对链路（后续已复现）

1. 具体设置和确认框的原文，是“结束游戏”还是“重新启动”。
2. 游戏是否调用 Steam URI、创建子进程或主动 ExitProcess；记录退出码与新进程身份。
3. CCL 对原进程的等待/registry 恢复、Android 对 launcher 退出的会话清理是否抢在后继进程之前。

原 CCL 只等待直接创建的游戏进程；默认 `Persistence.Mode=0`，退出即恢复 Steam registry。
Android 则按 Wine launcher 的结束事件退出 XServerScreen。源码存在交接需要复核的边界，
但不能只据此添加无条件自动重启，否则正常退出和真实崩溃可能变成重启循环。

使用 Reverse Study 对上述 SHA 的本地 EXE 做静态检查：原始 imagebase `0x140000000`，
80,462 个函数被索引。导入包含 `ExitProcess`、`TerminateProcess`，没有直接导入的
`CreateProcess` / `ShellExecute`；但 IAT 项也没有可用静态交叉引用，不能据此排除动态
解析或间接重启调用。未从静态结果推断执行路径，已关闭本轮独占分析 workspace。
该静态调查阶段没有启动 live debugger，也没有暂停游戏；后续客户端/Wine 改动见下文。

## 受控复现的前置过程

- 当前存档在设备外备份，SHA-256
  `4fde1618dd26e69a54ba0c958275f267eb9077ea53ca5734cc7d661d47280d94`。
  该备份包含用户后续游玩进度，不应以更早云端原件覆盖。
- 原画质配置：DX12 关闭、高分辨率材质包开启；完整配置已在设备外保存。
- 游戏中可见既有角色 MR 144 / HR 149。探索中不允许直接返回标题，故从任务菜单正常
  选择结束探索，进入报酬结算。15:47:46.332 系统 lowmemorykiller 因 critical pressure
  杀死 Android PID `5880`（oom_score_adj 50），随后游戏会话消失。这次没有触发画质重启，
  是另一个内存不足的中断，不能作为重启问题根因或重启验收。
- 保留该失败日志；重新启动标题菜单会话用于继续复现，避免加载探索场景。

仓库外原始证据位于 `xrgame-native-evidence/mhw/20260927/restart/`，包含客户端日志、
logcat 和现场截图；含私有数据的日志不入库。

## 明确复现与候选修复

用户在标题选项中触发重启后：

- 15:56:13.507 新 Wine 进程启动；旧游戏 Unix PID `21438` 结束，CCL 记录有效退出码
  `1`，立即恢复注册表并退出。15:56:26.354 的进程采样观察到后继游戏 PID `24310`。
- 后继 argv 直接指向 `C:\xrgame\games\582010\MonsterHunterWorld.exe`，没有 CCL；
  Android PID `17592`、Wine 启动进程 `21343` 仍在，故不能归因为 Android 抢先杀会话。
- 屏幕明确显示 `Steam Error / Application load error 3:0000065432`。后继 maps 没有
  Steam DLL。原启动调查也验证过关闭额外客户端 hook 会产生同一错误。
- 此次用户同时将 DX12 改为 On、高分辨率材质包改为 Off、TextureQuality 改为 FULL；
  已保存新旧配置。后续不得把用户的新选择自动覆盖为旧配置。

候选只对 MHW profile 启用：

1. Wine 的 `wine-image-bootstrap.patch` 在主程序 TLS 初始化后、entry point 前，按完整 EXE 路径
   精确匹配继承的 bootstrap 环境，初始化既有的 source-built client hook。其他 EXE
   不加载它；缺失或非绝对 DLL 路径会使匹配的程序启动失败。游戏 EXE/DLL 不变。
2. CCL 的 `gbe-loader-process-family.patch` 将初始游戏纳入继承的 job，保留 Steam
   注册表直至整个进程组结束，并观察同路径后继的退出码。不按退出码自动重启，不靠
   固定延迟猜测交接。原 remote injection 仍用于未选择此模式的 profile。
3. App 检查已验证 Proton 的 bootstrap capability；老运行时不能静默启用此模式。

### 候选 1

- APK：`144c32d0d84de13a9d615bce8adf44d8af57a1604c9349853d19bb501042abc7`。
- catalog：`e79f59fb2e3d6ef19e1fd1a4674925a987c613b8425dd52679a48815a18c3358`。
- Proton xrg2 archive：`205ea2dfd01b7245bea16e32c05851ad9bc55b8af42b16eafec0d375c6971043`。
- Steam validation8 archive：`51550f4d5d4278bed3c05b585a8a85513289801909390ca2342c04f88f3275ef`。
- Loader：`a239090b413e743b906101238c07510ddcd33992cb22fa747cf4dd95a7be561b`。
- ARM64X ntdll：`3dd0eb4c81a3aac0dd92047b9be62856704187d2abca5e12d54040b9f3f394ff`；
  i386 ntdll：`7155016f1d85c9f235cbfdf91b3153d88c834cb29355cb4eba48217237d46f8d`。

设备 fixture：`restart-probe.c` 编译为 EXE 和 bootstrap DLL。自重启轮 Wine PID
220 → 252，两者均在 entry point 前得到各自的初始化标记；无关 EXE PID 236 未注入。
原进程退出后，后继仍能打开存活的 CCL PID，最终 loader 返回 0，墙钟 4.749 秒。
正常退出轮返回 0（2.900 秒），主动错误轮返回 17（2.910 秒），均未重启。
这些是功能探针，未捕获每轮 Unix PID/start-time，不作为 debugger 身份验收。
临时替换的 runtime/prefix ntdll 已恢复原件，再安装 APK，以检查真实升级流程。

针对启动路径的 Kotlin 测试 16 项通过；候选 APK 审计 123 项、0 错误。
未提交或推送。原始失败、源码补丁、构建记录及 fixture 日志均保留。

候选 1 的 MHW 启动失败：Android PID `10530`，CCL `12394`，游戏 `12423`。
bootstrap 返回成功，但游戏在 CCL 启动后 1.040 秒以退出码 3 结束。该轮 TLS 前的
初始化时序没有通过真实游戏验收；同时游戏为用户新选择的 DX12 模式，未仅据退出码
认定具体原因。候选 2 把 bootstrap 移到 TLS 完成后，匹配原来的远程初始化时序。

候选 2 APK `af76f6c279689b04773867253fa795aebca17e3b61cc819c04824508479c7f7f`，
catalog `f4f34d00c5e207b65b583f4be1bedcd85a4ffffac6bd7e6c36a9443339c2159f`，
Proton xrg3 archive `9f3382ec66f86bfa8ba7ef2b96929b916b1b2d6d4c21bd96df90965e10f66dbc`。
APK 审计 123 项、0 错误。首轮启动在游戏开始前被设备 framework 重启打断：
16:23:40.658 watchdog 因 AMS/UI/display 等线程阻塞杀死 system_server `29284`；
Android app `16415` 随后收到 `DeadSystemException`。kernel boot id 未变。
保留该失败现场，另开 retry，不混入游戏重启结果。


### 后续诊断与完整候选

候选 2 的 retry 在 bootstrap 成功后仍以退出码 3 结束。`+loaddll/+debugstr`
记录显示 Steam API 加载 Windows 客户端后又进入 builtin `lsteamclient.dll`。
为绑定内置 Windows 客户端的 picoXr Steam 路径设置 `PROTON_DISABLE_LSTEAMCLIENT=1`
后，Android `28350` / CCL `29513` / 游戏 `29538` 在 16:28:56 启动并进入 ICEBORNE
标题；DX12 为 On。此前退出码 3 不能归因于 TLS 次序。

该会话由标题选项将 DX12 改为 Off 并确认重启后黑屏，旧游戏 leader 为 zombie，
另有 8 个线程残留，CCL 仍等待原进程，未观察到新游戏。此轮尚未到达待验的后继
Steam 初始化路径，不能记为重启通过。保留 logcat、线程 stat/maps、CCL 日志。
native-debugger 只读检查拒绝已空的 leader cmdline，也拒绝 package 不匹配的 Wine
线程身份，未创建调试会话；没有绕过身份检查。结束该已卡住的自有测试会话后继续复测。

完整候选 APK：`aa0f7bdd1ef65b7cc2cd36fc4a3e627db03cc4d6fa26d76bb52c080862e79928`，
catalog 和 runtime 同候选 2；123 项 APK 审计通过。移除容器临时 WINEDEBUG 和
PROTON_DISABLE 覆盖后，实际新游戏环境仍为 `PROTON_DISABLE_LSTEAMCLIENT=1`、
`WINEDEBUG=-all`，证明设置来自 App 启动路径。

TLS 增强 fixture 在已安装的 xrg3 上运行，不替换 runtime/prefix DLL。
bootstrap DLL 要求本进程 TLS callback 已设置当前 PID，防止继承旧进程标记假通过。
自重启 4.517 秒返回 0；正常退出 2.470 秒返回 0；主动错误 2.605 秒返回 17；
无关 EXE 未加载 bootstrap，后继观察到原 CCL 仍活跃。第一轮探针因 APK 安装路径
变动使旧 LD_PRELOAD 失效，保留失败输出；更新宿主库路径后才取得上述结果。


### 完整包受控重启

- 16:45:06 冷启动，Android PID `8089`、CCL `9324`、MHW `9348`。
- 标题选项 DX12 Off → On，选择“是”后，旧游戏退出码 1；CCL 未退出，
  16:50:00 后继 Unix PID `12885`（Wine PID 664）出现，bootstrap 返回成功。
  16:52:08 的截图确认回到 ICEBORNE 菜单，打开存档选择可见原 3 个角色，包括 MR 144 / HR 149。
  240 秒采样涵盖进程交接，其中后继连续观察至少 216 秒，未出现 3:0000065432。
  未加载探索场景。
- DX12 On → Off 的第二次重启也已观察到后继 Unix PID `16521`（Wine PID 1100），
  Android/CCL 仍为原 PID；17:04:13 截图确认回到 ICEBORNE 菜单。此前启用高量
  Wine trace 时的残留线程中断未在本轮复现，保留其证据，不据此宣称找到了该中断的根因。
- 17:09:14 从标题菜单选择“离开 / 是”。CCL 记录整个进程组共 3 个进程、
  `Game family exit=0`，再恢复注册表。相对 CCL 日志起点为 `1,448,313 ms`；
  Android 收到 `onGuestProgramTerminated` 后正常结束会话。只读进程表确认没有
  MHW、CCL、wineserver 或其他 Wine 进程；没有为这次退出执行 force-stop/kill。
  退出后的连续采样 248 秒内没有再次拉起游戏或 CCL。
- 最终包的四个 `.so` Build IDs 与本文开头一致；设备 build/boot id 未变。
  本轮 PID/start-time ticks为：Android `8089 / 128271553`，CCL `9324 / 128273556`，
  游戏初始 `9348 / 128273575`、后继 `12885 / 128302944`、`16521 / 128360613`。
  各值已与保留的 `/proc/PID/stat` 证据核对，不能将 Wine PID 当成 Unix PID。
- 最后一次正常退出在 DX11 模式。退出后只恢复 `DirectX12Enable=On`；读回文件与
  用户触发原故障后保存的整个 `graphics_option.ini` **逐字节一致**，即 DX12 On、
  高分辨率材质包 Off、TextureQuality FULL。没有替换存档或游戏 DLL，没有第三次重启。
- 临时容器日志/客户端环境覆盖已移除。两次 native-debugger 只读失败检查未创建
  会话；没有遗留 debugger、断点或端口转发。原始日志/截图、失败尝试和构建证据留在仓库外。

## 修复范围与后续

当前只为 MHW profile 启用精确 EXE bootstrap 与进程组等待；其他 profile 保持原路径。
内置 Windows Steam 客户端路线显式关闭 Proton 的 Linux 客户端重定向。修复同时进入
Wine/GBE 子仓维护分支及父仓可重放 patch/构建配方；未提交、推送或推进 gitlink。

此结果覆盖标题菜单设置自重启、重启后存档识别及正常退出；未重新验收长时间探索、
退出卡住的高量 trace 场景、联机、其他游戏自重启或 Swan。调试轮发生的 LMK/framework
中断及一次残留线程不能算已修复，也不能归因为当前补丁的某一行，仍需独立证据。
