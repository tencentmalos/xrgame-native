# WP1 验收记录：Steam 装游戏（前置）

- 日期：2026-09-24 起；2026-09-26 续验（进行中）
- 规格：[docs/specs/xrgame-native-v1.md](../specs/xrgame-native-v1.md) WP1、C3、C5
- 分支：`feature/malos/wp1-steam-install`（基于 WP0）
- 状态：**构建部分完成；最新 CI 单元测试超时，完整设备验收未完成**。用户决定设备验收**先在 AYN Thor 上做**，Swan 之后补测。2026-09-26 已只读检查 Thor 上的现有安装：Steam 标签显示 188 个库条目，安装根已有游戏目录，详见 §5.2；这些观察不代替下载、校验与完整库验收。

## 1. 改动

| 要求 | 实现 | 位置 |
|---|---|---|
| WP1-1 JavaSteam 去 SNAPSHOT | Gradle init script 在不修改 `references/JavaSteam` 的前提下，把版本改为 `1.8.0.1-26-xrg.<gitlink 前 8 位>`，并发布到仓库内的 `build/javasteam-maven`。`settings.gradle.kts` 删除 Sonatype snapshots 仓库，改为只从该目录解析 `io.github.joshuatam`（`exclusiveContent`）。catalog 版本与 gitlink 不一致时脚本报错退出 | `tools/build-javasteam.{sh,ps1}`、`tools/javasteam/publish-local.init.gradle`、`settings.gradle.kts`、`gradle/libs.versions.toml:15` |
| WP1-2 `libgndownload.so` 源码构建 | picoXr 的每个 variant 用 cargo-ndk 从 `app/src/main/cpp/gn-download/rust` 构建 arm64-v8a 版本，经 `variant.sources.jniLibs.addGeneratedSourceDirectory` 覆盖 main 里的预编译件。通过 `--config` 追加 `--build-id=sha1`，crate 自带的 soname 与 16 KB max-page-size 保留。工具链版本写入 `app/build/outputs/gndownload/<variant>/BUILD_INFO.txt` | `app/build.gradle.kts` 末尾的 picoXr 分块 |
| WP1-3 外部存储安装根 | picoXr manifest 声明 `MANAGE_EXTERNAL_STORAGE`。每次安装后首次进入 MainActivity 时，`XrGameStorage` 依次：显示非官方客户端提示 → 申请"所有文件访问权限" → 创建 `/sdcard/XRGameNative/Steam/steamapps/common` → 把 `PrefManager.useExternalStorage` / `externalStoragePath` 指向 `/sdcard/XRGameNative`（每次安装只设一次）。`SteamAppScreen` 的存储权限检查不再对 picoXr 放行 | `app/src/picoXr/AndroidManifest.xml`、`xrgame/XrGameStorage.kt`、`SteamAppScreen.kt:1073-1075`、`PluviaApp.kt:80-81` |
| WP1-6 无上游服务依赖 | 进程级出网白名单 `XrGameEgress`：替换默认 `ProxySelector`，把白名单外的主机导向回环地址上一个关闭的端口（请求立即失败，也不会解析目标域名），每个被拦主机记一条 `XrGameEgress` 日志。白名单只含 Valve 域名 | `xrgame/XrGameEgress.kt`、`XrGameEgressTest.kt`（5 个用例） |
| WP1-9 账号安全提示 | 首次进入时弹出，不可取消；中英文文案 | `res/values{,-zh-rCN}/strings_xrgame.xml` |
| CI | 检出 `references/JavaSteam`，安装 Rust 1.98.1 与 cargo-ndk 4.1.2、NDK 27.3，先跑 `tools/build-javasteam.sh`，再 assemble 与单元测试。APK SHA 与 `libgndownload.so` 的 Build ID 同时写到日志和 job summary | `.github/workflows/xrgame-picoxr.yml` |

所有 picoXr 行为都由 `BuildConfig.XRGAME` 控制，只有 picoXr 为 `true`。上游 flavor 的行为不变，仍打包预编译的 `libgndownload.so`。

## 2. 构建证据（本机 Windows 11）

### 2.1 JavaSteam（WP1-1）

| 项 | 值 |
|---|---|
| 源码 | `references/JavaSteam` @ `433f2ad15c36d5e690a4fe77401ec3f6b960641e`（joshuatam `gamenative-latest`） |
| 构建 | JavaSteam 自带的 Gradle 8.12 wrapper，JDK 17.0.12；`BUILD SUCCESSFUL in 3m 39s`。构建后 `git -C references/JavaSteam status --short` 为空 |
| 版本 | `1.8.0.1-26-xrg.433f2ad1` |
| `javasteam-1.8.0.1-26-xrg.433f2ad1.jar` | SHA-256 `a96df299ad19f0211040093f00e21ae3ddde81e011da86c40d7cb778d183603c` |
| `javasteam-depotdownloader-1.8.0.1-26-xrg.433f2ad1.jar` | SHA-256 `6c9f3a7efdc7e81ddf31699ec6ede38acdc1a7becdb66774f01f7eae184dac66` |
| 与原 SNAPSHOT 对照 | Gradle 缓存中的 `1.8.0.1-26-SNAPSHOT` jar（`javasteam` `57d9e5b9…`，`depotdownloader` `9113fc20…`）与本地构建的 `.class` 清单完全一致：6099 / 6099、75 / 75，没有任何一边多出的类。jar 字节不同：不同 JDK 编译，且 Windows 检出的 `.proto` 资源为 CRLF，每个大约多 0.4 到 1 KB |
| app 依赖解析 | `:app:dependencies --configuration picoXrDebugRuntimeClasspath` 中只有 `io.github.joshuatam:javasteam{,-depotdownloader}:1.8.0.1-26-xrg.433f2ad1`，没有 SNAPSHOT |

### 2.2 `libgndownload.so`（WP1-2）

| 项 | 值 |
|---|---|
| 工具链 | rustc / cargo 1.98.1（`48a229cea` 2026-09-01），cargo-ndk 4.1.2，NDK 27.3.13750724（clang 18.0.4 r522817d），`-P 26`，`--locked` |
| Build ID | `b4dc542ecc60e7c302e877a952e3ed362b73058c`。在 scratchpad 单独构建与在 Gradle 任务中构建，两次得到相同的 Build ID |
| soname / 对齐 | `libgndownload.so`；LOAD 段按 `0x4000` 对齐（与上游预编译件相同） |
| 上游预编译件对照 | `app/src/main/jniLibs/arm64-v8a/libgndownload.so`：rustc 1.98.0，**没有 Build ID**，同样的 NDK clang |
| APK 内 | `lib/arm64-v8a/libgndownload.so` 的 Build ID 为 `b4dc542e…`（打包时 strip 过，SHA-256 `5e040a2e39c4e7cea38a141fb70aa7756f1870b0cd37ac0aa8f5dcc415037651`），说明生成目录的优先级高于 main 的预编译件 |

### 2.3 APK

| 项 | 值 |
|---|---|
| 源码 | `2df02789` + 本记录 §1 的未提交改动（提交后补 SHA） |
| APK | `app-picoXr-debug.apk`，从头打包，236,499,739 字节 |
| APK SHA-256 | `f3495a5842287681ea5a2328e048095ec997b07338f06bae45d2bc8f6b26981a` |
| 权限 | `aapt2 dump permissions`：`MANAGE_EXTERNAL_STORAGE`；`READ/WRITE_EXTERNAL_STORAGE`（maxSdkVersion 29，来自 main） |

### 2.4 macOS 接续环境（2026-09-26）

- 从 SSH 克隆 WP1 分支，远端最新与本机 HEAD 均为 `ca045ed33132de7582a87cc51c3eb8f4ed94b84c`；配置 `upstream`，只初始化 `references/JavaSteam` 与 `references/DepotDownloader`，未更新 gitlink。
- 主机 macOS ARM64，已有 JDK 17.0.12、Android SDK platform 36、build-tools 36.0.0、NDK 27.3.13750724。gitignored 的 `local.properties` 仅含 SDK 路径。
- `bash tools/build-javasteam.sh` 首次因 Maven Central 的 TLS 握手中断失败（1m 36s）；重试成功（1m 46s），两个子模块 `git status --short` 均为空。
- 本机 `javasteam` jar SHA-256：`98d2442d0405f3edc2ec5400c8af15d31b999bba610168473bc40e58f13f8596`；`javasteam-depotdownloader` jar：`f8192228823e5719878012a2b6cb37d90446ae14775ac52dda45f172474c2e45`；版本均为 `1.8.0.1-26-xrg.433f2ad1`。
- 提前执行的 Gradle 测试任务 dry-run 因 JavaSteam 尚未发布而失败；不是测试断言失败。失败与重试日志保存在仓库外 `wp1/20260926-ci/`。

## 3. 失败与偏差记录

1. **独立 Gradle 脚本方案失败**：第一次把 picoXr 构建逻辑放在 `app/xrgame.gradle.kts`，用 `apply(from=…)` 引入，编译报错 `Unresolved reference: android`。原因是脚本插件看不到 `plugins {}` 加载的 AGP 类。改为放在 `app/build.gradle.kts` 末尾的一个分块内。
2. **`java.io` 被遮蔽**：Kotlin DSL 中 `java.io.ByteArrayOutputStream` 解析失败（`java` 是扩展访问器），改用 `File.outputStream()`。
3. **exec 关闭输出流**：同一个 OutputStream 给两次 `execOps.exec` 用，报 `Stream Closed`。改为每次 exec 写一个临时文件。
4. **Windows 命令行吃掉双引号**：`--config target...rustflags=["-C",...]` 在 Windows 上传给 cargo 时丢了引号，报 TOML 解析错误。改用单引号字面量 `['-C','link-arg=-Wl,--build-id=sha1']`。
5. **与 spec WP1-1 措辞的偏差**：spec 写的是"把 `app/build.gradle.kts` 的本地构建开关 `localBuild` 改指 `references/JavaSteam`"。没有采用：该开关直接引用 jar 文件，不带传递依赖（JavaSteam 需要的 ktor-client-websockets 等不在 app 的 `javasteam-dev` bundle 里），而且路径指向仓库外的 `../../JavaSteam`。改为发布到本地 Maven 仓：依赖元数据（`.module` / `.pom`）与原 SNAPSHOT 的解析方式一致，`localBuild` 开关原样保留、不启用。
6. **增量打包导致 APK 膨胀**：WP1 第一次成功构建的 APK 为 245,990,947 字节。条目逐项对比只多了约 16 KB（压缩后），多出的约 9.5 MB 是 zip 中未被条目占用的空隙：AGP 增量打包在原 APK 上反复更新留下的。删掉 APK 重新打包后为 236,499,739 字节。因此验收用的 APK 一律从头打包（已写入 `AGENTS.md`）。

### 3.1 WP1 首次 CI 失败（保留）

Run [35984631826](https://github.com/tencentmalos/xrgame-native/actions/runs/35984631826)（commit `8e07ab41`，ubuntu-latest）：JavaSteam 源码构建、cargo-ndk、assemble 都成功。APK SHA-256 `bebb2673f226fddcaae748237a52b327140739b3fc3a24f1515e087989ffaf66`；`libgndownload.so` Build ID `1ec3be11d1b6f31b64b929dd26cd3ea97524349a`，与本机 Windows 构建的 `b4dc542e…` 不同。同一主机上重复构建结果一致，但跨主机不是逐位可复现。

`testPicoXrDebugUnitTest`：1365 个完成，3 个失败，5 个跳过（WP0 的两次 CI 没有这些失败）：

| 测试 | 原因 | 处理 |
|---|---|---|
| `ManifestIdCorrelationTest.manifestIdsMatchInstalledIds` | 本机复现的报错为 `Driver install failed for Turnip Gen8 V34: Download failed: Failed to connect to /127.0.0.1:9`：测试会逐项下载上游 manifest 中的组件，被 `XrGameEgress` 拦下。这也说明 Robolectric 会执行 `PluviaApp.onCreate`，白名单在单元测试中生效 | 在 `app/build.gradle.kts` 末尾的 picoXr 分块中，对 picoXr 的单元测试任务排除这个测试（它与 C3 冲突），不改上游测试文件 |
| `ContainerFilesDownloaderTest.testObsoleteArchiveCleanupKeepsCurrentAndUnrelatedFiles`、`testBlockingWrapperFunction`（本机是 `testCachedComponentReuse`） | 测试刚写入的缓存文件被删掉。`PluviaApp.onCreate` 在后台启动 `preloadAllContainerFiles`，下载失败时执行 `destFile.delete()`（`ContainerFilesDownloader.kt:106-108`），路径与测试文件相同。picoXr 下下载被白名单立即拦下，这次删除就与测试的执行重叠了 | picoXr 启动时不再预下载容器文件（这些文件来自 `downloads.gamenative.app`，C3 禁止使用；WP3 改由本仓 manifest 提供）。对设备的影响：有效的缓存文件会先被复用，失败路径只删除未下完的目标文件，所以此前在设备上不会误删有效缓存 |

**以上两处修复已提交，但还没有在本地测试通过**：本地复测被中断，以推送后的 CI 结果为准。

### 3.2 第二次 CI：单元测试超时（2026-09-26 查询，失败保留）

Run [35988070277](https://github.com/tencentmalos/xrgame-native/actions/runs/35988070277)，commit `ca045ed33132de7582a87cc51c3eb8f4ed94b84c`，ubuntu-latest，结果 `failure`。

| 项 | 实际结果 |
|---|---|
| JavaSteam 源码构建、Rust/cargo-ndk、APK assemble | 均成功 |
| APK SHA-256（来自 CI 日志，未下载 APK） | `570c127bfb4860cd06e21edb7425ae8cd7c27f0ac05cacc49740d3fb27c2a898` |
| `libgndownload.so` SHA-256 / Build ID | `15b48ec7adee7ada4527bb58b49bd6ee55977ef6c1f8281783cff59ebd8d7b24` / `1ec3be11d1b6f31b64b929dd26cd3ea97524349a` |
| 测试步骤 | 2026-09-24 10:47:17 UTC 开始，11:32:30 UTC 报 `The action 'Unit tests (picoXr)' has timed out after 45 minutes.` |
| 最后两个 STARTED | 10:50:23 UTC：`GOGDownloadManagerTest > gen1_download_includes_game_and_support_files`；10:50:24 UTC：`gen2_download_includes_game_and_support_files` |
| 结论边界 | 没有完整测试汇总，不能将 §3.1 的修复记为已全部通过。日志只能定位最后启动的测试，不能单凭日志确定挂起根因 |

**更正旧结论**：WP0 §4.2 中“挂起只出现在 Windows 主机”的判断不再成立；本次 Linux CI 也在同一测试附近挂起。旧失败记录保留。

原始日志保存在仓库外 `xrgame-native-evidence/wp1/20260926-ci/run-35988070277.log`，附 `SHA256SUMS`。本次查询 Actions workflow 仅有本 fork 的 `picoXr build check`；五个上游 workflow 尚未注册。

### 3.3 本机复现与测试隔离修复（2026-09-26，macOS）

- 未修改测试代码时，单独执行 `:app:testPicoXrDebugUnitTest --tests app.gamenative.service.gog.GOGDownloadManagerTest`：13 个测试全部通过，XML 测试时长 21.268 秒；含首次应用编译的 Gradle 总耗时 3m 47s。
- 随后执行原始全量 `:app:testPicoXrDebugUnitTest`：日志有 690 条 STARTED，最后同样停在 GOG 的 Gen 2 用例。抓取 test worker 的 `jstack` 后主动结束该 worker，Gradle 以 exit 143 失败，总耗时 2m 18s；不是完整测试结果。
- 线程栈直接证据：Robolectric 主线程停在 `GOGDownloadManagerTest.setUp:57 → PrefManager.init:70 → getPref:130 → runBlocking`；另有 **63 个** `FrontendSyncManager.syncGame:167 → PrefManager.getPref:130` 调用等待；这些任务来自安装状态事件监听器。
- 代码对应：`PluviaApp.kt:220` 的静态 dispatcher 随测试沙箱复用；每次应用初始化都在 `FrontendSyncManager.kt:93` 注册监听器；GOG 成功安装后在 `GOGDownloadManager.kt:684` 发送安装事件。结合“单独通过、全量挂起”的对照，判断为跨测试残留监听器触发大量后台偏好读取，导致后续初始化无法继续。CI 没有线程栈，因此 CI 与本机同因仍是推断。
- 修复仅在 `GOGDownloadManagerTest.setUp` 开头清除静态事件订阅，让使用普通 `Application` 的下载单测不触发前面测试留下的真实应用同步任务。保留 GOG 测试和真实 DataStore，不改生产下载逻辑。
- 原始全量复现还出现 `ModMaterializerTest.filterUnapprovedConflicts_ignoresPreviouslyApprovedInstalledAndOriginalFiles` 的断言失败；它在本次修复前即存在，与 GOG 挂起分开记录。

证据：仓库外 `wp1/20260926-ci/` 中 `gog-baseline.log/xml`、`full-baseline.log`、`full-baseline-thread-dump.txt`。

**修复后复测（失败也保留）**

| 轮次 | 结果 |
|---|---|
| 清理 GOG 残留监听器后全量 | 3m 12s 跑完 1364 个测试，7 失败、7 跳过；不再挂起，GOG 13 项与容器文件 23 项均无失败。失败为 Mod 1 项、EA 端口绑定 2 项、BestConfig 1 项、CommunityConfig 2 项、XrGameEgress 1 项 |
| 修正出网测试断言后定向复测七个测试类 | 16 秒跑完 120 个测试，1 失败、5 跳过；只剩原有 Mod 断言失败。EA 4 项、BestConfig 38 项和 CommunityConfig 3 项在该轮无失败，前一轮的这些失败未再次出现 |
| Mod 单项使用无目录别名的临时路径 | 通过，Gradle 9 秒。`JAVA_TOOL_OPTIONS=-Djava.io.tmpdir=<仓库外证据目录>/tmp`；测试源码未修改。系统默认 `/var` 是指向 `private/var` 的符号链接，而 `WindowsPathIdentity.absoluteKey` 使用绝对路径、不解析该链接，推断这是该主机失败的路径差异来源 |
| 两个测试修复 + 无目录别名临时路径，全量最终复测 | 3m 1s 跑完 1364 个测试，5 失败、7 跳过。GOG 13 项、XrGameEgress 5 项、ContainerFilesDownloader 23 项（其中 2 跳过）均无失败；ModMaterializer 34 项全部通过 |

出网测试修正：`XrGameEgress` 对允许的域名本来就委托给安装前的 `ProxySelector`，但旧断言强制要求 `DIRECT`，在本机配置了系统 HTTP 代理时失败。测试改为断言允许请求的代理路由保持原样；禁止域名仍必须进入回环端口 9。生产白名单实现未改。

最终剩余五项尚未解决：EA 两项 `BindException: Address already in use`；BestConfig 的 Hades2 Adreno735 配置返回空；CommunityConfig 两项字段断言失败。后三项在定向测试通过、全量重复失败，尚未确定共享状态影响的具体来源，不能宣称本机完整测试通过。最终汇总见 `full-final-summary.json`，日志与各测试 XML 见 `full-final.log`、`full-final-results/`。

各轮日志和 XML 位于上述仓库外证据目录；两个测试修复尚未提交，新的 Linux CI 必须在用户批准 commit/push 后才能验证。

## 4. 许可发现（转 WP2）

`app/src/main/cpp/gn-download/rust/src/store_dl/steam/base64.rs` 与 `crypto.rs` 与 `references/WinNative/app/src/main/cpp/wn-steam-client/rust/src/` 下的同名文件逐字节相同（SHA-256 比较）。`gn-download` 目录内没有 WinNative / wnsteam 的署名；`THIRD_PARTY_NOTICES` 只在第 378 行、Winlator 的上下文里提到 WinNative。两边都是 GPL-3.0（WinNative 的 `Cargo.toml` 为 GPL-3.0-or-later），许可兼容，但需要补署名。列入 WP2 的 `THIRD_PARTY_NOTICES` 补全。除这两个文件外的"整体分叉"关系只是推断（模块命名与依赖相近），没有逐文件核实。

## 5. 设备验收（待做）

出口判据逐项对应，结果在设备测试后填写：

| 判据 | 设备 | 状态 |
|---|---|---|
| QR 登录成功，游戏库完整显示 | Swan | 待做（需要测试账号） |
| 下载两款游戏到外部安装根（<2 GB 与 ≥10 GB），大的那款中途杀进程后续传完成 | Swan | 待做 |
| 两款游戏"校验文件"都通过，且不触发重新下载 | Swan | 待做 |
| 与 DepotDownloader 逐文件 SHA-256 一致（至少一款） | Swan + PC | 待做。DepotDownloader 3.4.0 已从 `references/DepotDownloader` @ `989f37b1` 构建到 `C:\workspace\xrgame-native-evidence\tools\DepotDownloader-989f37b1\` |
| adb push 的已安装游戏被导入并识别为 Steam 游戏 | Swan | 待做（方案见 §6） |
| 卸载重装 APK 后已装游戏仍被识别 | Swan | 待做 |
| DNS / 连接记录中除 Valve 外无其他远端 | Swan | 待做（root 下按 UID 抓取连接） |
| AYN Thor 冒烟与现状复查 | Thor | **部分完成**，见 §5.1、§5.2。现有 Steam 库已可见；QR 复测、库完整性核对和下载验收仍待做 |

### 5.1 AYN Thor 冒烟（2026-09-24，不登录 Steam）

用户同意在 Thor 上覆盖安装并授予权限。设备：AYN Thor，Android 13 / SDK 33，build `TKQ1.231222.001/eng.Thor.20260206.163241`，boot_id `8bb14501-5800-4b9b-a9ab-7173603812f7`（与 WP0 测试时相同，未重启）。

| 步骤 | 结果 | 证据 |
|---|---|---|
| 覆盖安装 APK `f3495a5842287681ea5a2328e048095ec997b07338f06bae45d2bc8f6b26981a`（即 §2.3） | `Success`，lastUpdateTime 18:00:38；安装前 `/sdcard/XRGameNative` 不存在，`MANAGE_EXTERNAL_STORAGE` 为 default | adb 输出 |
| 冷启动 | `am start -W`：COLD，TotalTime 934 ms，PID 20157 | adb 输出 |
| 出网白名单 | 18:00:50.331 `XrGameEgress: installed`；同一秒拦下 `downloads.gamenative.app`、`pub-9fcd5294bd0d4b85a9d73615bf98f3b5.r2.dev`（推断来自启动时的容器文件预下载 `ContainerFilesDownloader.preloadAllContainerFiles`：时间点吻合，且该调用在 `PluviaApp.onCreate` 中；日志本身不记录调用方）；进入游戏库后 18:02:19 拦下 `api.gamenative.app` | PID 20157 的 logcat |
| 登录页 | 显示 Steam QR 码，说明到 Valve CM 的连接与 QR 会话正常（CM 走 JavaSteam 的 WebSocket，不经 ProxySelector） | 截图 |
| 非官方客户端提示 | 中文文案，路径显示 `/storage/emulated/0/XRGameNative` | 截图 |
| 所有文件访问权限 | 点"继续"后直接打开本应用的"所有文件访问权限"页（图标与名称为 XRGame Native），打开开关后 `appops`：`MANAGE_EXTERNAL_STORAGE: allow` | 截图、adb 输出 |
| 安装根 | 返回后 18:01:46 日志 `install root set to /storage/emulated/0/XRGameNative`；`/sdcard/XRGameNative/Steam/steamapps/common` 已创建（`u0_a109:media_rw`，`drwxrws---`） | adb `ls -la` |
| 跳过登录 → 游戏库、"发现"页 | 游戏库空白但稳定；"发现"页先弹出上游的"游戏推荐"同意框（内容是向 GOG 分享游玩记录），点"暂不"后回到"全部"页。整个 PID 的 logcat（1249 行）中没有 `FATAL EXCEPTION` | 截图、logcat |

**发现的问题**

1. **提示框叠了两层（已修复，待设备复测）**：通知权限框弹出前后，MainActivity 各 resume 了一次，每次都显示一个提示框。点掉上面一层后，下面一层仍在。修复：`XrGameStorage` 记住当前对话框及其所属 activity，同一 activity 已在显示就不再弹；activity 销毁时关闭对话框；dismiss 回调只清除它自己那一个。修复后的 APK `03238d6935b881a9e5a9f32c854563b9e1346f018d638cbb5d4ac5346ed85425`（236,500,943 字节）已编译通过。需要一次全新安装才能复测，计划在 Swan 首次安装时进行，Thor 上不清数据。
2. **界面品牌**：登录页标题仍显示 "GameNative"（WP0 记录 §6 已列入 WP2）。
3. **上游"游戏推荐"功能**会向第三方（GOG）分享游玩记录，picoXr 应整体关闭，列入 WP2。

截图、PID 20157 的 logcat 保存在仓库外的 `C:\workspace\xrgame-native-evidence\wp1-20260924\`，附 `SHA256SUMS`。截图里有当时有效的 Steam 登录 QR，因此不提交到公开仓库。

### 5.2 AYN Thor 现有安装只读检查（2026-09-26）

用户授权连接 Thor 并检查现有 XRGame Native。未覆盖安装、卸载、清除数据或启动游戏下载。

| 项 | 观察与证据 |
|---|---|
| 设备 | AYN Thor；build `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；boot_id `8bb14501-5800-4b9b-a9ab-7173603812f7` |
| 运行身份 | 包 `com.tencentmalos.xrgamenative`，versionCode 23 / versionName 1.2.1，PID 6340；lastUpdateTime `2026-09-24 18:00:38` |
| 设备 APK 回读 | 236,499,739 字节；SHA-256 `f3495a5842287681ea5a2328e048095ec997b07338f06bae45d2bc8f6b26981a`，与 §2.3 早期构建一致；回读及提取总耗时 5.579 秒，不是下载吞吐测量 |
| APK 内 `libgndownload.so` | Build ID `b4dc542ecc60e7c302e877a952e3ed362b73058c`，由 NDK 27.3 `llvm-readelf -n` 读取 |
| Steam 游戏库 UI | ADB 原始截图显示 Steam 标签被选中，计数 188，能看到多个游戏封面；未核对服务端清单，不宣称库完整，也未重新执行 QR 登录 |
| 安装根 | `/sdcard/XRGameNative/Steam/steamapps/common/7 Billion Humans` 已存在；仅目录存在不足以证明安装完成或校验通过 |
| 存储与权限 | `df -h /sdcard` 显示可用 529G（工具的人类可读显示）；`appops` 的 UID 模式为 `MANAGE_EXTERNAL_STORAGE: allow` |
| 截图失败记录 | scrcpy one-shot 返回 PNG 且会话已清理，但图像全黑；改用 `adb exec-out screencap -p` 得到游戏库界面。两份原始截图均保留，未提交 |

原始证据位于仓库外 `xrgame-native-evidence/wp1/20260926-thor/`，附 `SHA256SUMS`。`device-state.txt`、`package-state.txt` 含各次采集的 UTC 时间及实际耗时。设备序列号、APK 和整屏截图不入仓库。

## 6. 导入方案（待设备验证）

adb push 的目录放在 `/sdcard/XRGameNative/Steam/steamapps/common/<installdir>` 下，然后对该游戏执行"安装"：

- 没有 journal 时，`decide_depot_resume` 返回 `Download { trust_existing_chunks: false }`（`app/src/main/cpp/gn-download/rust/src/store_dl/steam/depot_downloader.rs:314-326`）。
- 磁盘上已有字节的文件进入校验候选，逐 chunk 用 `existing_chunk_matches` 比对：读出该 chunk 的原始字节，计算 Steam Adler32，与 depot manifest 中的 `crc` 比较（`depot_writer.rs:3003-3018`）。匹配的保留，缺失或不一致的才下载（`depot_writer.rs:1805, 2443`）。下载下来的 chunk 同样按大小 + Adler32 校验（`depot_chunk.rs:63-69`）。
- **更正**：此前在对话里和本记录初稿中写的是"SHA-1 比对"，这是错的。chunk 的 SHA-1 只用来拼 CDN URL（`depot_writer.rs:2533-2556`）。spec §2.4 与 `gn-download/rust/README.md` 中的"SHA-1 校验"也不准确。Adler32 不是密码学哈希，所以 WP1-7 用 DepotDownloader 做逐文件 SHA-256 交叉校验是必要的独立检查。
- 所以导入同时就完成了校验，而且不需要复制文件。上游 `importCustomGameAsSteamGame` 会把目录复制到 `CustomGames`，只按字节数检查（`CustomGameImporter.kt:107-110`），不作为 picoXr 的主路径。
- 限制：需要联网获取 manifest 与 depot key。离线导入仍然只能走上游那条不校验的路径。设备实测通过后，按此更新 spec WP1-5 的措辞（需要用户确认）。

2026-09-27 的 AYN 小游戏下载/校验、大目录续传准备修复与强制中断恢复，见
[个人版 P2 下载续验](personal-p2-downloads-20260927.md)。独立 DepotDownloader 对照仍未完成。


个人版 P2 的 AYN 原位导入机制已通过：Balatro 主机往返副本去掉 journal/完成标记后，
重新识别安装，19 个文件 SHA-256 保持一致；这不替代独立 PC Steam 原始副本测试。
公共 depot 228989 的 DepotDownloader 独立下载有 5/5 文件 SHA-256 一致，整款游戏
对照仍未完成。Balatro 匿名请求明确被 Valve 拒绝为账号无权限，未尝试提取应用令牌。
详见上述 P2 续验记录；表格中的 Swan 条件仍未验收。
