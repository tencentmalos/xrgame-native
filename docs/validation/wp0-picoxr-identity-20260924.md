# WP0 验收记录：picoXr flavor 与最小身份

- 日期：2026-09-24
- 规格：[docs/specs/xrgame-native-v1.md](../specs/xrgame-native-v1.md) WP0、C1、C4
- 分支：`feature/malos/wp0-picoxr`，基于 `malos/main` @ `bbb40faa`
- 状态：**部分通过**。本地构建与静态检查通过；CI 与设备两项待做，见 §4。

## 1. 改动

| 文件 | 内容 |
|---|---|
| `app/build.gradle.kts` | 新增 `picoXr` flavor：applicationId `com.tencentmalos.xrgamenative`，minSdk 29，targetSdk 36，只打 arm64-v8a，其余 BuildConfig 同 `modern`。源集同 `modern`：java 加 `src/nonXr/java`，assets 用 modern+main，jniLibs 加 `src/modern/jniLibs`。新增 `xrgame` 签名配置，只从 gitignored 的 `app/keystores/xrgame.properties` 读取。`androidComponents` 对 picoXr 禁用 `release-signed` / `release-gold`，`release` 改用 `xrgame` 签名 |
| `ubuntufs/build.gradle.kts` | dynamic feature 模块加同名 flavor 与相同的 variant 过滤 |
| `app/src/picoXr/AndroidManifest.xml` | 与 `src/modern/AndroidManifest.xml` 相同（移除 `REQUEST_INSTALL_PACKAGES`） |
| `app/src/picoXr/res/values*/strings.xml` | 覆盖 main 中定义 `app_name` 的全部 15 个目录（默认 + 14 个 locale），统一为 "XRGame Native" |
| `app/src/picoXr/res/mipmap-anydpi-v26/ic_launcher{,_round,_alt,_alt_round}.xml`、`res/drawable/ic_launcher_xr_{background,foreground}.xml` | 自有启动图标：橙色底（`#D9480F`）+ 白色头显剪影。上游为深蓝底（`#284561`，`app/src/main/res/values/ic_launcher_background.xml:3`）。alt 别名也用同一图标 |
| `.github/workflows/xrgame-picoxr.yml` | 本 fork CI：`assemblePicoXrDebug` + `testPicoXrDebugUnitTest`，不注入 secret，不上传 APK，只把 APK SHA-256 写进 job summary |
| `docs/upstream-sync.md` | 同步流程、冲突热点与首次演练记录 |

## 2. 构建身份

| 项 | 值 |
|---|---|
| 主机 | Windows 11 Pro 10.0.26200 |
| 工具链 | JDK 17.0.12（Oracle），Gradle 8.12.1（wrapper），AGP 8.8.0，build-tools 36.0.0 |
| 源码 | `bbb40faa` + 本记录 §1 的未提交改动（提交后在此补 commit SHA） |
| 命令 | `./gradlew --console=plain :app:assemblePicoXrDebug`，`BUILD SUCCESSFUL in 6m 15s` |
| 本地前提 | 仓库根目录的 `local.properties` 只含 `sdk.dir`，没有任何 secret（原因见 §5-1） |
| APK | `app/build/outputs/apk/picoXr/debug/app-picoXr-debug.apk`，236,485,280 字节 |
| **APK SHA-256** | `3201fe92f6935049ec0a496b584cadefd16d9e526a3b75ac206170b165333e8e` |
| 签名证书 | `CN=Android Debug`，证书 SHA-256 `ce9d5b4d6524b793e48e00658f4d6c9d2ccfa1b1059dbb1fb34244c68cd1f8eb`（本机 `~/.android/debug.keystore`） |
| 组件 manifest | debug 包内打入仓库根 `manifest.json`，SHA-256 `77cf40a6891789170087d61fc44bab65742152d1acb07f4a795abac6825dc877`（上游原样，WP3 替换） |

`aapt2 dump badging` 结果：

```text
package: name='com.tencentmalos.xrgamenative' versionCode='23' versionName='1.2.1' ... compileSdkVersion='36'
minSdkVersion:'29'
targetSdkVersion:'36'
application: label='XRGame Native' icon='res/mipmap-anydpi-v26/ic_launcher.xml'
native-code: 'arm64-v8a'
```

APK 中 16 个 `application-label*` 条目（默认 + 15 个 locale 限定）全部是 "XRGame Native"，没有残留 "GameNative"。versionCode / versionName 仍为上游的 23 / 1.2.1，WP0 未改。

`:app:signingReport` 中 picoXr 的 variant：

| Variant | 签名配置 | keystore |
|---|---|---|
| `picoXrDebug` | `debug` | `~/.android/debug.keystore` |
| `picoXrRelease` | `xrgame` | 未配置（`null`）：release 构建会在签名步骤显式失败，不回退到 debug key 或上游 `pluvia` key |

`:app:tasks --all` 中只剩 `assemblePicoXr{Debug,Release}` 两个 APK variant；`release-signed` 与 `release-gold` 已不存在。

### 2.1 `.so` 基线（APK 内 `lib/arm64-v8a`，共 31 个）

以下是打包后（AGP 已 strip）的文件，SHA 与仓内 `jniLibs` 中未 strip 的原件不同。例如仓内 `app/src/main/jniLibs/arm64-v8a/libgndownload.so` 的 SHA-256 为 `cce9ddec…`。Build ID 用 NDK 27.3 的 `llvm-readelf -n` 读取；"(none)" 表示文件里没有 GNU build-id note。

| 文件 | Build ID | SHA-256 |
|---|---|---|
| `lib7zx.so` | (none) | `a535e3fc9b2564b8cca670e2af7b11baeb7a101cf2cc80ec44cfd2f314ee6783` |
| `libahbimage.so` | `d6594980fbce21db9c851f8b89853e41dd0c45cd` | `25c2af0ff3a5180f30f937d650ccc68b4f5b5f55b48db4e45a18d73c5c784fb7` |
| `libandroidx.graphics.path.so` | `a7846ae06d6bf6cf63fa7c5e056b5de493c71567` | `41e9a793c43a0f4fddb19e33f346bace464f30f888ba7b9eaf96294ea115bfb6` |
| `libarchive-jni.so` | `d235438c5533e44a6e830f535018c72e8b43b117` | `92332de41e661c2a5ce05df65c8a0a6367a99a2cb201e3b3eec7039c9cbc669e` |
| `libasurface_renderer.so` | `aac6a9405b436a80ec50335241dac32eb05311d2` | `4323ea6909dd70eb49a0c4f6950f1113f01a9618c3ef82087f9e8ab3e13af9e3` |
| `libc++_shared.so` | `8889c70cf7388cc68030d32491527559beb5aaf5` | `4e2dcc0b0beae5c9dcbf25d90400a4de203cbffbfae6526d4be7befa88a77c09` |
| `libdatastore_shared_counter.so` | `17db37bd6770ac00dd2d1d2828839fd23a7959a3` | `d3e48717c9aa147e0ab21063ba0e8e0211cabf8bf40b222640829519edbf58e1` |
| `libevshim.so` | `95e1eafb5bbde724d6caf615ab645a162800acfe` | `e60e5de6fff65d2fa07cb0181d4c04d770e67efaede9bc190218b1d768c5b1b3` |
| `libextras.so` | `dcdcb9708ab389ebc9e9ed5ff900c5e2492e8f4f` | `d28624c60b9fa263d3861c035fe91220cddfdf7ea1ef16b2b3459e1885a35def` |
| `libgamenative_dns_v4mapped.so` | (none) | `e4d17e31e2f833f377d24784d50d25063539dcca5eb7f7bb83e93adf9d4cb86d` |
| `libgndownload.so` | (none) | `5c9022e37287ec78f0dae8a12d6ebe4cbd68807a77d66b4eee6f75d401832036` |
| `libhook_impl.so` | `0ae97778918a0b7460d2a8841ef2ccb2c9350268` | `03b90c37d97a6c8507e209a5e3ef2b248b7cfc4f06668697e20333d36a873c6d` |
| `libkgslshim.so` | (none) | `6e8f95027db2a363eea04e26352ebe113022cfb9d96b8cebd77ad24856ba7f0e` |
| `liblsfg-vk-layer.so` | `131d882613d89cafca09a9bc067ddd3a8be1f498` | `91faa290099f98d4fd2ec2cb0e8f0b83d44d714a31a11c2d139800e8e20bc8c7` |
| `libltdl.so` | (none) | `bc12038c8cedb7ba405b6621b579ad6b643e018b12f5f1d01e6cb78169e49907` |
| `libmain_hook.so` | `75b59e30485e5b89aaee1298d702e00a8d9247c8` | `9caad3feb27c936e1fe29e5e35b1fe7c7ba1f60927e0c06454b222d8e9b1805c` |
| `libopenxr_loader.so` | `84683adddc00d1d91778e2c1e89dddf07d7b2b6e` | `cd6c054dbc2c75d7d94d62c15267fb78d04cbdeeb4b8899fca01816f04620501` |
| `libpatchelf.so` | `a3eea5caac15435f9726197c18ce260eddf19ed3` | `53a2832cf8495349200fa353be2b656ff75198e7dbf0c47fa6507988fe3863b6` |
| `libpulse.so` | (none) | `061a0479989dd406924c837d7a09a59f086aec122d8b636b518da1658f225052` |
| `libpulseaudio.so` | (none) | `c70622e9dadb31aba51b36ca6313a944e0febd8beabea5ace4b9f42ce951d350` |
| `libpulsecommon-13.0.so` | (none) | `febdaa6fb344775f0c980c100e33046b27c9701d24346a45890b640380147a26` |
| `libpulsecore-13.0.so` | (none) | `1bbfb039b159da9b6c0090e3309e6f686b1468e8a4d77b8a61612fa3af3e576a` |
| `libsndfile.so` | (none) | `d6b61f5dddedce3cd29c5db9b58b5dd16a4b3faebfcbe925bc9432e2993831ee` |
| `libsteambootstrap.so` | `41dcf165bddf0f6cdfc43fc61305fcf12532444f` | `58a7473135e1960366146004ffe04f0e42d61e70e6a8122648318d87275f7f7e` |
| `libvirglrenderer.so` | `5bfcc74ba981937a274c90e2ee23176dec25a797` | `984d57ae1f208e858ef4f938465f4c958c01a6c62b43d16c27e1782664540673` |
| `libvortekrenderer.so` | (none) | `7c1c93d1c898b046a3dcb9f26845f3f3cbe535863e873221159169ae71a5c596` |
| `libvulkan_renderer.so` | `849371a71bbc14c9a19c8704395fecddba1a192f` | `649482bae3324c452025c09d6eb9f42bedec42e414cd1dcd2872ead8001a6c28` |
| `libwinlator.so` | `797a3afe26fe0cbe791160b1040bdeb7cf2d340b` | `98a8c057e4d77aa895e7d5008229ff7219a67e2a380b787fa53653ea36c51e99` |
| `libwinlator_11.so` | (none) | `1a1045dc8d00489be0957777adfa6f6d47bd58b6aa3a1ed05b33ba294dae0726` |
| `libxconnectorpatch.so` | `8280ec4f05b3fccaf5f20bf2be774c588735836b` | `0634934cd7ad96586f17fd0142fcb735b4b6df0e03f2ddb9a95fec04840d835b` |
| `libzstd-jni-1.5.7-5.so` | (none) | `9f4a912cb337867de6f729a544aaa91aaa8cf50e9dd06ae862e0a3dd16c5fdf0` |

另有 `assets/libredirect-bionic-wx.so` 与 `assets/libredirect-bionic-wx-minimal.so`（来自 `src/modern/assets`）。`libkgslshim`、`libsteambootstrap`、`libvortekrenderer`、`libredirect*` 属于 C2 清单，按 spec 在 WP2 / WP4 处理，WP0 保持与 `modern` 一致。

## 3. 静态检查：与上游 GameNative 共存

同一设备上装两个包，常见的安装冲突来源有：重复的 provider authority、重复的自定义 permission、sharedUserId。检查 `app/src/{main,modern,picoXr}/AndroidManifest.xml` 的结果：

- 唯一的 provider authority 是 `${applicationId}.fileprovider`（`app/src/main/AndroidManifest.xml:236`），随 applicationId 变化，不冲突。
- 没有 `<permission>` 定义，也没有 `sharedUserId`。
- 应用快捷方式 `LAUNCH_GAME` 用显式 class（`app/src/main/java/app/gamenative/utils/ShortcutUtils.kt:92-93`），不会被上游 app 接走。

不阻止安装、但两个 app 共享的项（v1 路径不用，记录备查）：

- 深链 scheme（均在 `app/src/main/AndroidManifest.xml`）：`home://pluvia`（:71-72）、`nxm://`（Nexus mod manager，:81）、`gamenative://discord-linked`（:91-92）、`gamenative://run`（:102）、`app.gamenative://oauth/callback`（Nexus OAuth，:130-132，另见 `NexusOAuthModels.kt:10`）。两个 app 同时安装时，打开这些链接会弹出选择框。

## 4. 出口判据

| 判据 | 结果 | 证据 |
|---|---|---|
| 干净 checkout、无 secret 时 `assemblePicoXrDebug` 成功 | **本地通过**。前提是存在一个只含 `sdk.dir` 的 `local.properties`（§5-1） | §2 |
| picoXr 单元测试 | 见 §4.1 | — |
| 本 fork CI 通过 | **待做**：workflow 已写好，需要推送后在 GitHub 上运行 | `.github/workflows/xrgame-picoxr.yml` |
| 可与上游 GameNative 同时安装，名称与图标可区分 | **静态通过**（§2、§3）。**设备待做**：需要在 Swan 上同时安装两个包并截图 | — |
| `upstream/master` 首次同步演练并记录冲突点 | **流程已演练，但还没遇到真实冲突**：2026-09-24 fetch 时 `upstream/master` 仍是 `ebde76e9`，merge 是空操作。冲突热点已按上游 90 天改动量列出 | [docs/upstream-sync.md](../upstream-sync.md) |

### 4.1 单元测试（本机 Windows 11；完整结果以 Linux CI 为准）

| 轮次 | 命令 | 结果 |
|---|---|---|
| 1（失败，保留） | `:app:testPicoXrDebugUnitTest` | 910 个完成，60 个失败，4 个跳过。57 个是 Robolectric `initializationError: Package targetSdkVersion=36 > maxSdkVersion=35`。原因：上游用 `app/src/testModern/resources/robolectric.properties`（`sdk=34`）给 modern 固定 Robolectric 4.14 的 SDK，picoXr 缺少对应文件。已补 `app/src/testPicoXr/resources/robolectric.properties`。其余 3 个见下一行 |
| 对照 | `:app:testModernDebugUnitTest --tests GOGConstantsTest --tests EpicCloudSavesTest` | 25 个完成，失败 3 个，与第 1 轮剩余的 3 个相同：`GOGConstantsTest` 的 `testSanitizationSpecialChars`、`testGetGameInstallPath_pathStructure`，`EpicCloudSavesTest` 的 LocalLow 用例。断言信息显示期望值是 `\` 分隔，实际是 `/` 分隔，属于 Windows 主机路径问题 |
| 2（挂起） | `:app:testPicoXrDebugUnitTest`（补 `sdk=34` 后） | Robolectric 测试真正开始运行，但在 `GOGDownloadManagerTest > gen2_download_includes_game_and_support_files` 处空转：test worker 5 秒墙钟用了约 5.7 CPU 秒，`jstack` 显示停在 `GOGDownloadManagerTest.setUp` 与 `FrontendSyncManager` 协程中反复调用 `PrefManager.getPref`（`PrefManager.kt:130`）。约 13 分钟后手动停止 |
| 3（全量对照，看门狗 180 秒无输出即停） | `:app:testModernDebugUnitTest`，然后 `:app:testPicoXrDebugUnitTest` | **两个 flavor 结果完全相同**：都启动 690 个测试、失败 51 个，STARTED 集合与 FAILED 集合逐条 `diff` 无差异，都挂在同一个 GOG 测试上。失败分布：`SteamAutoCloudTest` 37（`FileNotFoundException`）、`PathTypeTest` 9、`GOGConstantsTest` 2、`RegistryKeyFixTest` 2、`EpicCloudSavesTest` 1 |

结论：picoXr 相对 modern **没有引入测试回归**。本机的 51 个失败和那次挂起在 modern 上同样出现，都在涉及文件路径的测试类中。其中 3 个已确认是路径分隔符问题；其余 48 个与挂起的具体原因本机没有继续深挖。因为挂起，本机只跑了 910 个中的 690 个，完整结果以 CI（ubuntu-latest）为准。

## 5. 失败与偏差记录

1. **第一次构建失败**（保留）：`./gradlew :app:assemblePicoXrDebug` 在配置阶段失败，`BUILD FAILED in 1m 14s`，报错 `Failed to notify project evaluation listener. > The file 'C:\workspace\xrgame-native\local.properties' could not be found`。原因是 `com.google.android.libraries.mapsplatform.secrets-gradle-plugin` 2.0.1（`gradle/libs.versions.toml:133`，应用于 `app/build.gradle.kts:12`）要求文件存在。所有 manifest 占位符只有 `${applicationId}`、`${icon}`、`${screenOrientation}`，没有依赖这个插件注入的值。处理：本机新建只含 `sdk.dir` 的 `local.properties`（已被 `.gitignore:15` 忽略），CI 用 `touch local.properties`。上游 CI 也是写这个文件（`.github/workflows/pluvia-pr-check.yml`，写入 dummy PostHog 值）。
2. **禁用上游 workflow（用户已同意，未能执行）**：对 `app-release-signed.yml`、`tagged-release.yml`、`adhoc-signed-build.yml`、`issues-contributors-only.yml`、`pluvia-pr-check.yml` 执行 `gh workflow disable`，全部返回 `HTTP 404: workflow ... not found on the default branch`，而 `GET /repos/tencentmalos/xrgame-native/actions/workflows` 返回空列表。`GET .../actions/permissions` 返回 `enabled: true, allowed_actions: all`。推断：fork 的 workflow 在仓库 Actions 页点击启用之前不会注册，所以上游 workflow 现在不会运行，本仓的 `xrgame-picoxr.yml` 也可能不会运行。推送后按 CI 是否产生 run 来验证。在 Actions 页启用后，立即禁用上述 5 个。

## 6. 遗留清单（不属于 WP0 出口判据）

- **写死的上游包名路径**：换 applicationId 后会失效，只影响 Wine 启动（WP4），不影响 WP1 装游戏。
  - `Container.java:53-60`（MEDIACONV_* 与 `DEFAULT_DRIVES`）
  - `DXVKHelper.java:20`
  - `BionicProgramLauncherComponent.java:212-215`
  - `WineUtils.java:48, 60`
  - `evshim.c:86`（以及预编译的 `libevshim.so`）
  - `libkgslshim.so` 也含 `app.gamenative` 字符串
- **界面文案**：`app/src/main/res/values/strings.xml` 中还有 28 行含 "GameNative"。应用名以外的品牌文案放在 WP2 处理。
- **版本号**：picoXr 仍用上游的 versionCode 23 / versionName 1.2.1。自有版本方案在 WP3 与组件可追溯性一起定。
