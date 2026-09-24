# Android Steam 客户端实现对照（spec WP1-8）

本文对照三种实现在登录与 depot 下载上的差异：本仓当前实现、WinNative `wnsteam`、Pluvia。用途：如果 JavaSteam 在 Swan 上出现无法修复的登录或下载问题，按本文评估切换方案。v1 不预先切换。

- 日期：2026-09-24。依据是读源码，**没有在设备上运行过后两者**。
- 未标注的 `file:line` 来自代码阅读，读取时的 pin 见 [references/README.md](../references/README.md)。标"已复核"的条目另外逐行核对过。

路径缩写：

| 缩写 | 路径 |
|---|---|
| SS | `app/src/main/java/app/gamenative/service/SteamService.kt` |
| GDS | `app/src/main/java/app/gamenative/service/download/GameDownloadService.kt` |
| GN | `app/src/main/cpp/gn-download/rust/src/store_dl/steam/` |
| WN | `references/WinNative/app/src/main/cpp/wn-steam-client/rust/src/` |
| WK | `references/WinNative/app/src/main/feature/stores/steam/` |
| PL | `references/Pluvia/app/src/main/java/com/OxGames/Pluvia/` |
| JS | `references/JavaSteam/src/main/java/in/dragonbra/javasteam/` |

## 1. 关键发现

- **本仓的 Rust 下载引擎与 wnsteam 同源**（已复核部分）：GN 下的 `base64.rs`、`crypto.rs` 与 WN 下的同名文件逐字节相同（SHA-256 比较）。`gn-download` 内没有 WinNative 署名，需要在 WP2 补 `THIRD_PARTY_NOTICES`。两者都是 GPL-3.0。其余文件的"同源"关系只是推断（文件命名与 crate 依赖相近），没有逐个核对。
- **chunk 校验是大小 + Steam Adler32，不是 SHA-1**（已复核）：见 `GN/depot_chunk.rs:63-69`、`GN/depot_writer.rs:3003-3018`。chunk 的 SHA-1 只用来拼 CDN URL（`GN/depot_writer.rs:2533-2556`）。`gn-download/rust/README.md` 中"SHA-1 校验"的说法不准确。由于 Adler32 不是密码学哈希，WP1-7 用 DepotDownloader 做逐文件 SHA-256 比对，作为独立检查。

## 2. 登录

| | 本仓（JavaSteam） | wnsteam | Pluvia |
|---|---|---|---|
| 方式 | 用户名密码 + 设备确认 / 手机码 / 邮件码，经 `IAuthenticator`（`UserLoginViewModel.kt:36-78`，SS:3336-3338）；QR 轮询（SS:3392-3424）；两者都以 `logOn(accessToken = refreshToken)` 收尾（SS:3298-3310） | `BeginAuthSessionViaCredentials` + Steam Guard（DeviceCode / EmailCode / DeviceConfirmation，WN `jni.rs:2724-2959`）；QR（WN `jni.rs:3035-3158`）；Kotlin 侧 `WnAuthenticator` 与 `IAuthenticator` 形状相同（WK `wnsteam/WnAuthenticator.kt:5-10`） | 与本仓同源的 JavaSteam 流程（PL `ui/screen/login/LoginViewModel.kt:32-66`，`service/SteamService.kt:784, 830`） |
| 令牌存储 | DataStore，Android Keystore 密钥 `pluvia_secret`，AES-256-CBC（`PrefManager.kt:47, 838-866`） | EncryptedSharedPreferences，AES256-GCM（WK `utils/PrefManager.kt:43-56, 148-158`） | Keystore AES-CBC（PL `PrefManager.kt:360-387`） |
| 令牌轮换 | 无（`SteamWishlistService.kt:143` 传 `false`） | 剩余不足 7 天时轮换（WK `SteamService.kt:311`，`SteamServiceConnection.kt:340-370`） | 无 |
| 重连 | 1 s 起翻倍，最多 60 s，20 次（SS:360, 4383-4397） | 2 s 起翻倍，最多 5 min，20 次，稳定连接 60 s 后才重置重试计数（WK `service/SteamService.kt:300-306, 4354-4385`） | 立即重连，20 次（PL `SteamService.kt:210, 1257-1266`） |
| CM 传输 | 只用 WebSocket（SS:376），JavaSteam 用 Ktor CIO（JS `networking/steam3/WebSocketConnection.kt:59-70`） | WSS，tungstenite + rustls（WN `ws_connection.rs:8, 288`） | WebSocket |
| CM 发现 | 缓存的 `server_list.bin`（SS:552, 4139）+ `ISteamDirectory/GetCMListForConnect`（JS `SteamDirectory.kt:38`） | `GetCMListForConnect`，失败时用写死的 `ext1-*.steamserver.net` 列表（WN `cm_server_list.rs:134-145`） | 同本仓 |
| 其他前提 | — | 需要从 `/system/etc/security/cacerts` 生成 CA bundle，否则不启动会话（WK `wnsteam/CaBundleExtractor.kt:22`） | JavaSteam `1.6.1-SNAPSHOT`（`references/Pluvia/gradle/libs.versions.toml:31`） |

## 3. 游戏库

- **本仓**：license 列表写入 Room（SS:4917-4974）；PICS 按 `lastPICSChangeNumber` 增量轮询（SS:5026-5110）；product info 写入 `appDao`（SS:5120-5314）。
- **wnsteam**：Rust 端维护内存库，爬取 package / app PICS（WN `library_store.rs:50-220`），导出 JSON 快照（`:275`）；Kotlin 端写入 Room（WK `SteamServiceConnection.kt:530-635`，`SteamServiceFriendsChat.kt:283-490`）。
- **Pluvia**：与本仓相同的模式（PL `service/SteamService.kt:1495-1599, 1696-1736`）。

## 4. depot 下载

| | 本仓 | wnsteam | Pluvia |
|---|---|---|---|
| 分工 | JavaSteam 取 CDN 列表（GDS:352-367）、depot key（GDS:451-460）、manifest request code（GDS:390-411）、CDN auth token（GDS:272-286）；以 JSON plan 交给 Rust（GDS:139-150） | 全部在 Rust 内，走自己的 CM 连接（WN `cm_client.rs:720-770`） | JavaSteam `ContentDownloader`，全在 JVM（PL `SteamService.kt:494-503`） |
| chunk 处理 | AES-256 → VZip/LZMA、VSZa/zstd、PKZip（GN `depot_chunk.rs:36-58`）；大小 + Adler32 | 同样的 Adler32 检查（WN `depot_chunk.rs:24-48`） | 取决于解析到的 SNAPSHOT（推断） |
| 续传 / 已有文件 | `.DepotDownloader/` journal；干净暂停后信任已有 chunk，否则逐 chunk 重算 Adler32（GN `depot_downloader.rs:311-326`，`depot_writer.rs:3003-3018`） | 相同的 journal 与干净暂停逻辑（WN `depot_config.rs`，`depot_downloader.rs:211-224`） | — |
| 并发 | tokio，自适应 2–256 个请求，每 host 最多 8 个（GN `depot_writer.rs:120-184, 2279`）；后台测速对 CDN 排序（GN `cdn_probe.rs`） | 线程池，大小由 `downloadSpeed` 设置决定（WN `depot_writer.rs:347-369`） | 8 个许可的信号量（推断） |
| CDN auth token 重取 | 有 | **没有** | 未知 |

## 5. 非 Valve 端点（登录、库、下载路径内）

- 本仓：登录、库、depot 代码中没有发现（SS:1398-2042，GDS）。装游戏路径外的上游服务调用，由 `XrGameEgress` 在 picoXr 中拦截，见 WP1 验收记录。
- wnsteam：Rust 代码只涉及 Valve 主机；WinNative app 另外从 `github.com/maxjivi05`、`dl.winehq.org` 拉组件（WK `service/SteamService.kt:326`，`SteamClientManager.kt:480, 550`）。
- Pluvia：imagefs 走 Play Feature Delivery（PL `service/SteamService.kt:406-418`）。

## 6. 成熟度与许可

| | 本仓 | wnsteam / WinNative | Pluvia | JavaSteam fork |
|---|---|---|---|---|
| 最近提交 | 2026-09-24 | 2026-09-23（浅克隆） | 2026-02-23，README 自述已停滞 | 2026-08-02 |
| 测试 | GN 中 105 个 Rust `#[test]`；Kotlin 测试文件 175 个 | 180 个 Rust `#[test]`；Kotlin 测试文件 46 个 | 5 个文件 | 50 个文件 |
| 许可 | GPL-3.0 | GPL-3.0（crate 为 GPL-3.0-or-later） | GPL-3.0 | MIT |

## 7. 如果要从 JavaSteam 切到 wnsteam

- **JNI 导出名写死为** `com_winlator_cmod_feature_stores_steam_wnsteam_*`（WN `jni.rs:420` 起），需要改名或沿用该包路径。
- **要替换的本仓代码**：客户端配置与回调（SS:4136-4188, 4251-4262）、登录（SS:3251-3467）、连接与 logon 处理（SS:4353-4751）、license 与 PICS（SS:4917-5314）。登录 UI（`UserLoginViewModel.kt:36-78`）几乎可以直接换，因为 `WnAuthenticator` 与 `IAuthenticator` 同形。
- **下载路径**：`WnSteamSession` 只暴露 `downloadApp`，不向 Kotlin 暴露 key、code、server、CDN token 这些调用。二选一：
  - 改用它的 `downloadApp`，放弃本仓的自适应引擎、CDN 测速和 CDN token 重取；
  - 在 WN `cm_client.rs:720-770` 外加 JNI 导出，继续给 GN 喂 plan。
- **仍依赖 JavaSteam 的功能**：本仓有 32 个 Kotlin 文件 import JavaSteam（云存档、好友、家庭组、愿望单、成就统计）；Workshop 还用 `javasteam-depotdownloader`（`workshop/WorkshopManager.kt`）。
- **CA bundle**：wnsteam 需要，本仓下载 plan 目前传空路径（GDS:141）。

结论：切换的代价主要在 SteamService 的连接、登录与 PICS 部分，下载引擎本身已经同源。只有 JavaSteam 在 Swan 上登录或 CM 连接出现不可修复的问题时，才值得评估。
