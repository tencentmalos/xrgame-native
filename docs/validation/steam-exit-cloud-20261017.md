# Official Steam 游戏退出和 MHW 云存档

2026-10-17，`feature/steam_flow_fix` 基于 `malos/main` 的 `8c15fe11`。本轮处理普通 Android 上的游戏退出和官方 Steam Cloud，沿用现有显示及运行时。以下设备时间为 UTC+8；官方日志使用 UTC。

## 退出阻塞

AYN 的旧包 v42 在 16:15:43 从 MHW 标题菜单选择「离开」后，游戏窗口解除映射，`MonsterHunterWorld.exe` 随即消失。但 `start.exe`、`steam.exe`、Wine 桌面和服务仍存在，画面留在启动器的控制台窗口；16:16:41 再点 Android 快捷菜单退出才返回官方大屏。

`XServerScreen` 原来的窗口退出检测向 `winhandler.exe` 请求进程列表，picoXr 却没有启动这个辅助程序，因此检测等不到回复。官方 Proton 的 `steam.exe` 又等待 Wine 会话整体结束，不能用它仍存活来判定游戏仍在运行。

修复在 picoXr 的游戏窗口解除映射后读取同 UID 的 `/proc` 进程快照。游戏进程或其他业务子进程仍存在时继续等待；连续 3 秒只剩已知 Wine 和官方客户端辅助进程才执行原有会话退出流程。读不到完整进程快照会重置等待，不当作游戏结束。Custom 的 ColdClientLoader 仍作为业务进程保留；上游 UDP 路线保持原样。

退出仍由原有清理路径释放 X server、结束 Wine 会话、清除活跃游戏记录，再让官方兼容工具返回。Steam Cloud 由官方客户端接手，JavaSteam 不参与官方游戏的退出同步。

## 云存档

旧包的官方日志显示 MHW `AC Exit,Sync Disabled`，并记录本地 `SAVEDATA1000` 与云端有冲突。不能将这个状态解释为仍在上传。

用户明确选择「以这台设备最近的进度为准，上传覆盖云端」。先在私有制品目录备份原存档和官方元数据；退出基线测试后又备份了上传前的最新文件。通过官方大屏设置启用 Steam 云，在 MHW 冲突对话框选择「本地存档」。16:19:56 开始上传，16:20:03 官方日志记录 `Upload complete, result OK`，变更号从 246 到 247，界面显示「Steam 云：已是最新」。其他游戏的冲突没有代为选择。

这次恢复的是设备上已关闭的官方 Cloud 偏好；没有新增每次启动强制开云的代码，也没有引入第二套存档同步。两种模式继续共享同一物理存档目录。账户、原始日志、截图和存档备份不进入公开仓库。

## 文件验证的触发条件

Custom 模式手动更新或验证时，原生下载器使用 `fresh` 重新核对已有内容；中断续传也检查已有块，补齐不匹配内容。验证游戏文件不再隐式覆盖本地存档。

首次把已有 Custom 安装导出给 Official 时，原记录没有持久化 build ID，导出的 ACF 使用 `buildid=0`，让官方客户端决定核验或更新。已有官方 ACF 不会在每次启动被重写成零。

本次用户看到的「正在验证 59%」对应官方 `content_log.txt` 中约 2.019 GB 的 shader 缓存更新：15:22 排队，游戏运行期间暂停，15:52 退出后恢复，16:00 完成。本次日志没有显示全量校验 MHW 游戏文件；这项后台内容工作与 Cloud 上传分开进行。

## 验证范围

36 项定向 Kotlin 测试通过，覆盖退出状态机、Wine 进程选择和官方 Steam 集成。状态机包含游戏窗口隐藏、进程自重启、业务子进程延后退出、不完整快照和 Custom loader 场景。

v43 已保留数据安装，采用本机 Apple Container 构建，`picoXrDebug` / `RelWithDebInfo`，audit 142 文件、0 错误。与 v42 比较，APK 中 34 个 ARM64 原生库字节均相同；本次修改的是退出检测的 Kotlin 代码。

| 制品 | 身份 |
| --- | --- |
| APK SHA256 | `4b17d4f0f3af94164efc0a5dfdfcdd8b0d4d5881121aee8092ce64f5df9e0ff3` |
| 运行时 manifest SHA256 | `c389df3e8db71bf054362b188abdb2ecdf92454649eb94b24af8451fd9598943` |
| 官方客户端 manifest SHA256 | `222ad273606787b58fce697526ab5da14b03d6fe9fda106242cee70fd70077dc` |
| `libwinlator_11.so` Build ID | `d57c4d4612088a6e48212ad7c177b3a8294079b5` |
| `libproot.so` Build ID | `a6b8f3798a5dc7c927a86f58a742e373dc21c6a0` |
| `libxrgame_steam_session.so` Build ID | `ba722107785de2182eaca5ae4611f552695b4d5c` |
| `libxrimmersive.so` Build ID | `c23f6204e53b9e4c8ed85626e3e7b925f622c3a0` |

设备是 AYN Thor，Android 13/API 33，4 KiB，build `eng.Thor.20260206.163241`，boot `b509848d-21a5-4f1d-b6ed-24c0572aa9d2`。新 app PID 24821，官方 PRoot 25267，Steam 25280，MHW 26248；16:28:06 官方启动同步确认已到云端变更号 247，没有再次询问冲突。

本轮从官方启动到退出同步结束共约 4 分钟。MHW 到达标题菜单后选择「离开」，16:31:58.028 窗口解除映射并启动检测，16:31:58.792 的进程快照已不含游戏，但仍有 `start.exe`、`steam.exe` 和 wineserver。16:32:03.139 自动走 `game_processes_exited`，比窗口消失晚 5.111 秒；下一次快照已没有这些 Wine 进程。没有调用快捷菜单退出。

16:32:04 官方兼容工具以 0 返回，Steam 移除运行状态并执行 `up,AC Exit`；16:32:07 记录 `Upload complete, result OK`，`SAVEDATA1000` 的变更号从 247 到 248。前台是原 `LinuxSteamActivity`；同一官方 Steam 进程继续运行，JavaSteam 的 running、connected、loggedIn 均为 false。官方大屏处于自身的空闲壁纸，另行导航到 MHW 详情页核对云状态；这次 URI 导航没有参与游戏退出或 Cloud 触发。

已验证 MHW 标题菜单的正常退出与退出后自动上传。状态机的窗口隐藏、自重启和子进程保护由单测覆盖；尚未把它扩展为所有游戏、长时间游玩或 Swan 的生命周期验收。

旧包基线、失败截图和上传前存档均保存在私有 `steam-flow-fix` 制品目录。
