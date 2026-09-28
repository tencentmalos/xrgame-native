# P2 Steam 下载续验（AYN，2026-09-27）

状态：**小游戏下载与原文件校验通过；修复大目录准备停滞后，暂停/强制中断续传已恢复，大游戏完整安装与整文件校验通过**。
依据 [personal v2](../specs/xrgame-native-personal-v2.md)，Swan/XR 与发行不在本轮范围。
原始证据在仓库外 `personal/20260926/ayn/p2-download/`，设备完整身份、boot ID、
进程启动 tick 和 APK 的全部 `.so` Build ID 见相邻 JSON。未保存账号凭据。

## 构建身份

- 第一轮 APK：`2a0a1e7ffd323bd0d86bec316a00cd8079b405668d3f419dc25f97833003cec7`。
- 修正验证/准备流程 APK：`32564ffff5914b0bac270369babaf941b44f36970fa21ca9ca5b21a114c91378`。
- 两者均为 122 条目审计、零错误，内置 catalog SHA：
  `260213a29a2d57087273b8edb1a8171c8d6aaaf29604bea8e47e3ecdb5bdb1a7`。
- `libgndownload.so` Build ID：`45a2263934179eae4cd7bc536e977519eeab61ae`。
- AYN Thor / Android 13 / API 33 / 4 KiB，targetSdk 36；不能替代 Android 16 结论。
- 首轮 app PID 26779；冷启动续传 PID 5274，完整 generation 在采集 JSON 中。

## Balatro：完整小游戏下载与校验

专用测试账号的现有库内游戏，app 2379780；首次安装 UI 为 57.40 MiB 下载 / 63.57 MiB
游戏本体。实际流程还下载公共运行库，结束累计解压文件字节数 **92337448**。
两 depot：228989 / manifest 5753583882400741046，2379781 / manifest 3512319404653808464。

- 02:06:49.723 注册下载，02:07:13.689 Rust 回报完成：**23.966 秒**（含准备阶段）。
- Native 日志记录 `*.steamcontent.com` CDN 实际传输；两 depot 完成，零跳过。
- UI 变为“开始游戏 / 已安装”；文件位于外部安装根的 `Balatro` 目录。
- 02:08:54.493 发起“验证文件”，02:08:56.923 完成：**2.430 秒**。
  两 depot 的该次传输计数为 0，校验前后逐文件 SHA-256 完全一致。
- 首轮只验证已有正确文件；后续损坏修复与公共运行库独立副本对照见下文。
  `.manifest` 是本客户端缓存，不能冒充独立来源。

证据：`balatro-result.log`、`balatro-{before,after}-verify.sha256`、`balatro-depots.txt`、
`start.json`、`balatro-complete.json`、截图 113–119。

## AI Limit：暂停与冷启动续传

库内 app 2407270；UI 提示 19.85 GiB 下载 / 22.30 GiB 安装，足以覆盖大文件下载场景。
[Steam 官方页面](https://store.steampowered.com/app/2407270/AI_LIMIT/) 标为单人游戏；
本轮仅做下载，不据此宣称游戏运行兼容或完成反作弊二进制审计。

- 初次 CDN 传输持续增加；手动暂停后 UI 显示“继续下载”。下载环境有屏保，
  首次触摸只退出屏保，早期数次单击未触发暂停；未把它算作 downloader 故障。
- 暂停后强制停止并冷启动，仍显示“继续下载”；日志恢复 **6516771985 字节**。
- PID 5274 于 02:18:27.759 注册续传，随后观测长期停在“准备仓库 (1/1)”，当时没有取得
  native 传输日志。此轮未完成，保留停滞和调试失败证据；后续实物文件量表明不能把
  整段观察都算成零传输（见下文）。
- `/proc` 线程观测不足以判定是 CM 等待还是 native 前的文件操作；源码加入阶段日志与
  30 秒网络准备期限，随后复测，不把超时保护称为根因修复。

## 发现并修正的行为

- picoXr 的“验证文件”不再显式调用 `forceSyncUserFiles`；安装完成阶段也不再自动选择
  Remote 存档。云恢复保留独立入口，文件校验不隐式覆盖本地进度。中英文提示同步修改。
- Steam CDN 列表和每个 depot 的准备请求分别受 30 秒期限约束，所创建异步请求属于
  该期限作用域；取消继续传播，不转成下载失败。日志只记录 app/depot/阶段，不打印密钥。
- 下载失败给出可重试提示，保留下载进度。网络期限不覆盖 native 文件处理，也不能中断
  不可取消的同步文件系统调用；需要后续阶段日志证明实际停在哪里。
- 5 项定向测试通过：可空返回、超时关闭子任务、用户取消、嵌套期限、普通错误。
  初版两个测试错误地要求异常对象同一引用，另一个依赖协程异常文本；修正为行为断言。
  失败日志保留，最终构建 3 秒成功。

## 调试器异常（独立保留）

会话 `69b55358b9b040e88c750e7a5b7dd174` 在 PID 5274 上 attach 后报告退出 -1，
但设备上的同一 PID/start tick 仍存在，TracerPid 为 0；不能把它记为目标崩溃。
第一次 cleanup 返回 `target_exit_not_observed_on_device`。保留证据后停止已卡住的应用，
再次 stop 返回 `target_exited`，设备上无本会话 server 或 forward 残留。

原始 `native-stall-cleanup/protocol/dap-000.ndjson` SHA：
`c101a792a9bce83f349fec664ffc1ee5008fda0a1bbda7936e607a9fd21b5d94`；
控制流水 SHA：`739ff41a867b5e2e6b9ed2a8b6fe0f69f8060a7e353f7405f5537f90c31dfe91`。
02:28:20 的 exited/terminated 事件是 debugger 报告；后续设备探测是存活反证。
本轮没有得到目标线程栈，保留此分析限制。

## 大目录续传根因与修复

后续 APK `32564f…1378` 的阶段日志确认：CM 的 server/depot 请求正常返回，进入
native 后停在文件准备。进程的打开 fd 指向含大量文件的 `StandaloneWindows64`
目录，持续没有进入 fetch-window；源码中两轮准备都会对每个缺失文件重新 `read_dir`
查找大小写，造成近似平方级目录遍历。该目录实际已有 1258 个文件。

- 新增一次准备范围内的目录索引；精确路径优先，缺失文件共享父目录索引。
  每轮准备/每 depot 重新创建索引，保留大小写合并语义。
- 准备循环检查取消；debug 日志记录文件数、准备时长与已有字节数。
- 新 APK `84efcaaec66300f31135a2079576f525776761a7cb9176ccf805b75dfdedc5c2`，
  122 条目审计零错误，catalog 同上。安装后的 base.apk SHA 已核对。
- PID 24964：02:50:22.815 开始 3107 个文件的准备，02:50:23.924 完成，**1109 ms**；
  已有 **12368503595 字节**。因此早一轮确实曾在未取得持续日志的时段继续写入，不能
  将先前的停滞描述为整个会话始终零传输，也没有证据把 UI 12.34 GB 认定为重复计数。
- 校验已有文件后，02:50:42 开始新的 CDN 传输。
- 在实际传输中强制停止应用，冷启动为 PID 27898；02:52:33.776–34.978 的路径准备
  **1201 ms**，已有 **13475177274 字节**，随后再次校验并恢复 CDN 下载。
- 这些是准备时间及恢复证据，尚不代替最终全文件完整性结果。

新增 2048 个缺失兄弟文件的测试验证父目录仅扫描一次。Steam writer 52 项测试通过；
首次在 macOS 上有一个旧测试把大小写别名 `exists()` 误作重复目录，现改为检查实际目录
条目名。保留首次失败与修正后日志。记录在 `path-{before-interrupt,after-interrupt}`、
`new-stall` 与 Rust 测试日志中。

启动时另外观察到 CuratedListRemote 尝试请求 `downloads.gamenative.app`，被现有拦截器
改为本机拒绝连接。picoXr 已在调用入口直接返回空结果，避免该无效请求；定向测试通过，
保留数据更新后，冷启动 PID 5399 的检查窗口未再出现对应拦截/连接失败日志。这一项不能证明所有 app 出口均已穷尽审计。

本次 libgndownload 身份：`674e470eca24f48e67e09f5329fc7e7a58b98469`。

## 完整下载与整文件校验结果

- PID 27898 的中断恢复于 **03:02:46.967** 完成，native 回报 **23945945151 字节**、
  1 个 depot 完成、0 跳过。自 02:52:31.079 注册续传到完成为 **615.888 秒**，
  含准备、已有文件校验及剩余传输；不是从零下载耗时。UI 为“已安装 / 开始游戏”。
- depot **2407271**，manifest **3772203960994697774**，缓存 manifest SHA-256
  `e0287b6c40b5489e707ec6458279e53efa28092173a250a6e659d87bdda1f1b8`。
- 停止下载后在 app 身份下运行整文件 sha1sum，耗时 **27.837 秒**，再由独立 Python
  读取器对照缓存 manifest：**3050/3050 文件通过**，缺失 0，内容不符 0。
  其中 3015 项直接匹配 manifest SHA-1，35 个零字节文件的 manifest hash 为全零；
  对这 35 项显式核对 SHA-1(empty)，没有忽略内容。初版未处理该编码而报告 35 项不符，
  该失败报告保留；补入空文件正反例后再通过。唯一额外文件是安装完成标记。
- Balatro depot 2379781 也通过 **14/14** 整文件核对；额外文件是另一个公共运行库 depot
  和安装标记。工具的 5 项定向测试通过，包括缺失/改动、截断、路径越界、大小写重复和空文件。
- `check-steam-manifest.py` **不验证 Valve 签名，不构成独立 DepotDownloader 副本对照**。
  本轮也未启动 AI Limit，不能由文件正确推出运行兼容性。

证据：`ai-complete.json`、`path-full-resume.log`、`ai-content.sha1`、
`ai-content-duration.json`、`ai-content-check{,2}.json`、`balatro-content-check.json` 与截图 141。


合并包 `29c10bae9737be05d7f3be78bdded2de4f0431fbf63a0ff31e86be405521ec2a`
保留数据安装成功；122 条目审计零错误，catalog 与 libgndownload Build ID 同上，
设备 base.apk SHA 一致。更新后 AI Limit 仍显示已安装，截图 143；没有重新下载或清数据。


## 单字节损坏修复与目录重新识别

仍用合并 APK，app PID 17284。先将刚下载的 Balatro `readme.txt` 备份到仓库外，
仅翻转偏移 0 的一个 bit，文件保持 4711 字节。原 SHA-1
`a3b8f8c84bc7a12514f5521958bc5e43b9abf4cd`，改动后为
`8ba42afbfbc05e4f486ae0e1297169451768eef2`。
应用内“验证文件”实际使用一个 CDN server 修复该 depot，03:25:19.873 完成。
修复后文件恢复原 SHA-1，全部 20 项清单（含安装标记）与改动前完全一致。
没有用本地备份替代下载器修复。证据：`p2-repair/`、截图 155–158。

目录导入采用 WP1 §6 的原位验证方案：

- 将游戏及尚未试玩的容器分别备份到主机；移除本次新装的 Balatro 安装登记。
- 主机副本去掉 `.DepotDownloader` 和 `.download_complete`，留下 19 个实际文件，
  共 92337448 字节，通过 ADB 放回外部 Steam `common/Balatro`。
- UI 识别到已有目录并显示“继续下载”。03:28:53.256 注册，03:29:00.591 完成，
  **7.335 秒**；两 depot 日志均为 used=0/22 CDN servers、0 MB，按 manifest 检查已有内容。
- 主机副本和恢复安装后的 19 个文件逐文件 SHA-256 一致。恢复此前容器元数据后冷启动，
  UI 仍显示“已安装 / 开始游戏”（截图 163）。自动启动过渡画面被保留，未把它算作游戏运行验收。

这是本客户端下载内容经主机落盘后的目录往返测试，验证无 journal 的原位导入机制；
不冒充来自独立 PC Steam 客户端的副本。主机原始备份保留，账号和已有 Hades 数据未清除。
证据：`p2-import/` 与截图 159–163。

## DepotDownloader 公共运行库独立对照

从子仓固定 `989f37b1fbc012798bf41bdf0e66a60adf785ae5` 构建 DepotDownloader 3.4.0，
使用独立 .NET SDK 9.0.318，零警告零错误，未修改子仓源码。匿名登录下载 owning app
**228980** / depot **228989** / manifest **5753583882400741046**，未读取应用登录凭据。
下载 25108528 字节，解压 25674515 字节；一次 chunk 超时重试后成功。

独立副本的 **5/5 文件 SHA-256** 与应用为 Balatro 下载的相同公共 depot 一致。
这关闭了公共运行库的独立对照，但**整款付费游戏的 DepotDownloader 对照仍未完成**。
证据：`p2-download/depotdownloader-{build,anonymous}.log`、
`depotdownloader-redist-compare.json`；下载产物和临时请求码仅保存在仓库外。


同一构建随后尝试匿名获取 Balatro depot 2379781；Valve 明确返回该 app 对匿名账号
不可用，日志 `depotdownloader-balatro-anonymous.log` 保留。公共 depot 成功不能扩大成
付费游戏授权，也未从设备提取 Steam 凭据以绕过交互登录。
