# SteamPSP 联机支持与脱机提示核查

日期：2026-10-16。范围：最新 `malos/main` 源码与 AYN 已存配置的只读核查；不是联机验收，也未安装新 APK。

## 结论

当前 picoXr 的 Steam 登录、游戏下载和 Cloud 由 Android 侧 JavaSteam 等实现；Windows x64 游戏侧使用捆绑的 **GBE 离线客户端**。前者在线不代表后者接入了官方 Steam 服务。最新主干仍没有启用真实客户端桥接，所以游戏内报告脱机符合当前实现。

`offline=0` 只能改变 GBE 对游戏报告的状态，`disable_networking=0` 可以放开其本地网络实现；两者都不会自动创建 Valve 官方认证会话。需要区分 GBE 局域网互联、游戏自有服务器，以及 Steam 官方好友/大厅/匹配/认证。

## 代码与设备依据

源码基线：`78eb268aee54fcf12d5bcc6b7ead00ab502d596a`。GBE gitlink：`e6dab9c5a70dd0aefd29e704b4ee557921874569`。

| 依据 | 具体行为 |
|---|---|
| [XServerScreen](../../app/src/main/java/app/gamenative/ui/screen/xserver/XServerScreen.kt)，`BuildConfig.XRGAME && isSteamGame` 分支 | x64 PE 游戏经过 `XrGameSteamClient.prepare`；设置 `PROTON_DISABLE_LSTEAMCLIENT=1`，关闭 Proton 的 Linux 客户端重定向 |
| [XrGameSteamLaunch](../../app/src/main/java/app/gamenative/xrgame/XrGameSteamLaunch.kt)，`usesBundledClient` | 检查 PE machine 为 AMD64；不能把本结论无条件扩大到所有自定义程序/32 位路径 |
| [XrGameSteamClient](../../app/src/main/java/app/gamenative/xrgame/XrGameSteamClient.kt)，`prepare` | 每次启动写 `offline=1`、`disable_networking=1`、`disable_sharing_stats_with_gameserver=1`，不读取容器的 Steam offline 开关来决定这些值；手动改生成文件会被下次启动覆盖 |
| [GBE Steam_User](../../references/gbe_fork/dll/steam_user.cpp)，`BLoggedOn` | 返回 `!settings->is_offline()`；把配置改为 online 不等于真正连上 Valve |
| [GBE Networking](../../references/gbe_fork/dll/network.cpp)，构造函数及 `Run` | 禁用 socket 时不建 UDP/TCP 网络 socket，仍保留进程内消息投递；这是 Alyx 离线本地 IPC 修复，不是远程联机 |
| [Container](../../app/src/main/java/com/winlator/container/Container.java)，`isLaunchRealSteam` / `isLaunchBionicSteam` | 对 `BuildConfig.XRGAME` 强制返回 false；继承的 upstream Real/Headless/Bionic Steam 代码存在，不表示 picoXr 已接通 |
| [XrGameEgress](../../app/src/main/java/app/gamenative/xrgame/XrGameEgress.kt) | Java HTTP 客户端的出站路由限制，不是覆盖 Wine 原生 socket 的全设备防火墙；不能据此认定游戏所有网络都被统一拦截 |

AYN Thor / Android 13（API 33）上的包为 versionName `1.2.1`、versionCode `23`、targetSdk `36`，系统记录最后更新时间 `2026-10-04 22:07:22`。检查时无应用 PID、无 DebugBus 服务，未启动游戏。MHW `582010`、MHR `1446780`、AI LIMIT `2407270`、Alyx `546560`、Hades II `1145350` 的已存 `configs.main.ini` 均为上述离线配置。多个历史 runtime hash 目录可能共存，不能仅凭目录存在推断哪份 DLL 最近被加载。

Android 报告有默认网络和带 `INTERNET/VALIDATED` 的网络条目。这不证明 Steam 账号会话当前有效，但与“设备整体没网络”是不同问题。没有读取登录凭据、修改存档或向游戏服务器发起测试。本次未取得游戏中错误提示或实时认证日志。

## 当前支持范围

| 功能 | picoXr 当前情况 |
|---|---|
| Steam 登录、库、下载、启动前后 Cloud | 有 Android 侧实现和历史验证；本次未验证当前账号在线状态 |
| 游戏内离线 Steam API / 本地 IPC | 捆绑 GBE 路线；已用于现有游戏运行 |
| GBE 局域网联机 | 相关网络实现存在，但当前启动配置禁用；未做双设备验收 |
| 官方 Steam 好友邀请、大厅、匹配、认证及中继 | 当前游戏侧路线未接通，不能靠关闭 offline 标志解决 |
| 游戏自身的独立网络服务 | 依游戏而定；上述 GBE 设置不是 Wine 全局断网开关，仍需逐游戏检查 |
| VAC 或第三方反作弊游戏 | 无当前栈的兼容性/账号风险验收，不能作普遍支持承诺 |

Valve 的 [BLoggedOn 文档](https://partner.steamgames.com/doc/api/ISteamUser#BLoggedOn) 定义的是客户端与 Steam 服务器的实际连接状态。官方 [认证流程](https://partner.steamgames.com/doc/features/auth) 还要求会话票据及后端验证；报告一个在线布尔值并不能完成这些步骤。

## DroidDeck 能否解决

在此前审阅的 DroidDeck `2503d6929332de2a4a22c8a31c1b3ce84e5a8831` 中，[安装脚本](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-install) 下载 Valve Linux ARM64 客户端，[兼容工具配置](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/steam-compatibility) 使用客户端下载的 ARM64 Proton，[启动包装](../../references/DroidDeck/tools/linuxfs/overlay/usr/local/bin/droiddeck-proton-run) 使用 Steam 所选工具和 `STEAM_COMPAT_*` 环境。

**这条路线具备接入官方 Steam 在线服务的基础，消除了我们当前“游戏侧没有真实客户端”的障碍。** 但本次没有运行 DroidDeck，也没有 MHW/MHR 联机结果；不能将客户端在线等同于每个游戏通过认证、匹配、实际联机或反作弊检查。官方二进制来源也不意味着 Valve 支持 DroidDeck 的 Android/PRoot 组合。

如果目标是官方联机，值得评估真实客户端路线，而不是将 GBE 状态改成在线。是否保留 Bionic Wine、如何桥接 glibc Steam、是否需要独立 glibc Proton 环境，见 [DroidDeck 路线复核](droiddeck-comparison-20261013.md)。这与是否迁移显示系统无必然绑定。

## 风险与最小验证

- **兼容性**：真实客户端、Proton/FEX、游戏 Steam API 版本、P2P/中继和会话恢复都需接通；能登录客户端不构成联机成功。
- **存档与账号身份**：现有 GBE 使用与 Cloud 对齐的 SteamID。迁移须核对 prefix、userdata、DLC 和保存路径，并避免两套 Cloud 同时写入。
- **反作弊与账号**：Valve 提供游戏封禁机制，发行商也可采用自己的方案，见 [官方说明](https://partner.steamgames.com/doc/features/anticheat)。这里没有证据证明本组合一定会封号，也没有依据承诺安全。Wine/FEX、DLL 注入、guest patch 和调试器必须按具体游戏评估；首次验证不应把现有诊断注入带入受保护的公开服务器。

候选最小验证：按既有仓库要求使用专用测试账号及合法拥有、无反作弊的游戏，先确认游戏加载真实客户端桥，完成认证回调；再验证好友/大厅、两端实际进场及正常退出，最后检查存档/Cloud 和断线重连。局域网方案另列验收，不计作官方 Steam 联机成功。尚未实施。

## 本次仓库更新

`feature/malos/swan-xr-ahb` 已 rebase 到 `origin/malos/main@78eb268a`；原 HEAD `dc877ba1` 已是主干祖先，结果相当于快进，不需冲突解决。Foundation 与 OpenComposite 已按新 gitlink 检出，未使用 `--remote`。原 DroidDeck 引用、调研文档及其暂存状态已逐文件哈希/补丁校验恢复。未推送分支、未构建或安装 APK、未改变联机配置。
