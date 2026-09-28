# WP2/WP3 构建进展（2026-09-26）

**状态：进行中，未验收。** 本记录区分构建、打包与设备执行；已在 AYANEO Pocket DS 上跑通真实 Windows x64 7-Zip。尚未完成 W3 全部出口判据，没有发布 Release 或 commit/push。

**执行优先级更新（2026-09-26）：** 用户改为个人学习路线，原 WP3 收尾已移到
[新规划 P5](../specs/xrgame-native-personal-v2.md) 作为最后的可选项。
本记录保留已完成与未完成的事实；完整工程化/公开发行条件不再阻塞 AYN 上的运行验证。

Hades II 的后续黑屏修复、AHB/GPU 呈现和退出重启冒烟见 [Hades II 验证记录](wp3-hades2-20260926.md)；该结果不替代本记录仍未完成的 WP3 验收项。

## 已完成的构建

- macOS ARM64：picoXr 的 `winlator`/`winlator_11`、`extras`/adrenotools、`evshim`、DNS 库、Vulkan/Surface 合成器、AHB、xconnector 与 `xrimmersive` 已从源码构建；OpenXR loader/headers 来自官方 1.1.61 AAR。Rust `libgndownload` 继续源码构建。
- Windows OpenXR x64/x86 PE runtime 与 bionic unixlib 已由 NDK 27 在 macOS 构建。Wine builtin 由 Proton 11 构建；修正了旧 bridge 的 i386 调用约定及 ARM64EC dispatch 声明。Proton 11 的 makedep 自行生成 ARM64X flags，构建副本不再把旧的全局 `-marm64x` 用于 i386。
- Linux x86_64 Ubuntu 24.04 容器：Proton 11、FEX、DXVK、VKD3D-Proton、Turnip 均完成源码构建。工作目录为数据盘 `/data00/xrgame-native`，容器内 `/work`。
- FEX Wine 模块的 4 KiB 适配只在构建副本应用；本次不支持 16 KiB Windows 路径。FEX unixlib 使用自研、MIT 的 bionic 共享内存适配。宿主测试覆盖跨进程可见性、unlink 后映射继续有效、路径穿越和符号链接拒绝，已通过。
- Turnip 同时启用 Android 与 X11/XCB WSI。仅 Android WSI 的第一轮构建不能满足 Wine X11 路径，因此已补建；最终包使用补建产物。
- libxcb 1.17.0 从源码重建，X11 socket 改为 XRGame 包名路径；基础 imagefs 收集 112 个库、81 个 Termux 包，134 个库/字体文件通过逐文件哈希复核。许可文档随包保留，对应源码归档仍待补齐。
- PulseAudio/AAudio sink、libpulse/libpulsecore/libpulsecommon、libsndfile/libltdl 从固定源码重建；Android sevenzx 使用仓库 LZMA SDK 源码。旧 runtime、Steam host/input 和 PulseAudio 预编译资源不再进入 picoXr APK。

| 组件 | 源码 pin |
|---|---|
| Proton Wine | `5d0d333e7beef02fbb455cc3163b4e8e25615458` |
| ntsync-android | `7ce6435e5979b1cb5341aa4b299f31e8937fe121` |
| FEX | `3f1f30a060b633980ed8e7674eb8d8997457edad` |
| DXVK（Proton gitlink） | `a6764047e587178283fcde4073ae6e1410af594f` |
| VKD3D-Proton（Proton gitlink） | `212991fc2c266bc0d59f4c4ce8f80f7126508d71` |
| Mesa Turnip | `d15b7c019c8daa17e80051258077d9b2d5146a2b` |

没有改变 reference gitlink。FEX 的 `libarm64ecfex.dll` 经 llvm-readobj 识别为 `COFF-ARM64EC`，含 CHPE metadata；不能仅凭普通 `file` 显示的 x86-64 就认为它不是 ARM64EC。

## 本地组件包

`tools/xrgame/package-components.py` 从构建输出生成 WCP、adrenotools ZIP、逐文件身份记录及 draft catalog。它不发布、不修改 APK 内的 catalog。Wine 包补入 `bin/wine`/`wine-preloader` 相对链接；prefixPack 为空，已由 Wine 在 AYANEO 上完成首次 prefix 初始化。PE 保留构建字节，避免 llvm-strip 对 Wine synthetic debug directory 的不兼容。

| 文件 | SHA-256 |
|---|---|
| `proton-11.0-2-arm64ec-xrg1.wcp` | `d632fafcef484c0e8e6c634950a7f7d8e063336b7a60ef47fdc206b50822ea5e` |
| `fexcore-2608-3f1f30a-xrg1.wcp` | `6476dba1b8f1574618e72c4f532e773c5714fc86c655cb59e5878769c0e062ef` |
| `dxvk-11.0-a676404-arm64ec-xrg1.wcp` | `01d7153b93a46976105b8888e488998d7e099e151cbe3e5f8cd201a774444ce6` |
| `vkd3d-proton-11.0-212991f-arm64ec-xrg1.wcp` | `e50a148e0abbcd3321a1bf6cc3912b9463298b47ded1a0c35eeedbe3f7cb0b42` |
| `turnip-d15b7c0-xrg1.zip` | `20670521d62ec441d94016bb7bacfe6f16afb970c4f979d87ede67a3b5cbc1cb` |
| `imagefs_bionic.txz` | `38eb75e4b43d8ebf3a4b1c0bd93bf3dd44c8d1c79fc45212a054526262c095ba` |

Draft catalog SHA：`21d68778558ce61eb4192d7bb199b737599bf6c76315af3a7748f85a332a45f6`。其中 Release URL 为拟发布位置，目前不可用于下载。包内已有顶层许可；完整依赖声明和对应源码归档尚未完成，构建记录明确 `releaseReady: false`。

五个压缩包通过独立内容检查：包 SHA、记录中的逐文件 SHA/大小、链接链完整性、Wine loader/XR builtin、FEX DLL/unixlib 安装目标及 D3D/驱动必需文件均相符。此检查不执行 ARM64 或 Windows 代码，不代表运行兼容性。

## app 与本地验证

- 默认运行时已切换为 Proton 11 ARM64EC / FEX 2608（上述 pin）/ 自建 DXVK、VKD3D、Turnip。修复 Wine profile 名称解析；安装 FEX 的 PE builtin 和 bionic unixlib，并设置 `WINEDLLPATH` 让 Proton 能关联 unixlib。
- 安装和启动均使用本仓组件目录。归档 SHA 验证后记录安装凭据，后续启动校验凭据、文件哈希和链接范围。同名旧安装不被默认为可信。复制失败会明确中止。
- 新 prefix 写入有效 Wine registry 头；Wine 自行初始化系统文件。自定义 Windows 应用直接启动，文件管理器使用 Wine 自带 `winefile.exe`。完整 Steam 客户端/WinHandler 输入链路不在本次离线验证结论内。
- picoXr 关闭上游 API、调试上报、更新、Play Integrity、遥测、Steam DLL 替换及 headless/bionic Steam 模式，并移出 Samsung SDK。生产 catalog 仍为空。
- 内部 debug 构建可用 `-PxrGameValidationCatalog=/absolute/path/manifest.json` 嵌入私有验证 catalog；release 不接收该覆盖。debug 可从 app 私有目录 `files/xrgame/components/` 读取归档，但仍要求与 APK catalog 的 SHA 完全相符。未发布的 Release URL 不用于真实下载。
- `tools/audit-apk` 对最初验证 APK 的 **114 个 native/asset 文件检查通过，0 错误**，含来源、哈希/Build ID 和禁止项。官方 AAR 与 NDK 依赖登记固定哈希；zstd JNI 的 AGP strip 结果从原 AAR 独立复现。审计成功不等于设备兼容或完整发行许可审计通过。
- 设备调试前的 APK 与全量测试联合任务通过：**1370 项，0 失败，0 错误，6 跳过，166 suites，总任务耗时 2m03s**；日志 `final-validation-build-test.log`。覆盖 Wine 名称解析、已安装文件损坏和链接越界。此前单测轮耗时 2m13s；Linux CI 尚未运行。

### 私有验证产物

- 最终设备验证 APK SHA-256：`6ea1c1e20a633cd50c7827e7c023bccbc3f455b139da5e24839abe08fd9e5c8c`（282,035,375 bytes，先移走旧输出再打包），内置六组件；`apk-audit-device-final.json`：**120 项，0 错误**。
- 初始验证 APK SHA-256：`70a67dd9a45da2367c1676fd652c5f4988f0bef0af9eac0268ae8dda9769629d`，保留用于重现早期失败。
- 六组件验证 catalog SHA-256：`43968bfd94fdcba985b0350a72fed36ff0ad27143e786dbd4152b7edf4811b17`。
- AYANEO 首次安装后，调试迭代均使用保留数据的覆盖安装；未卸载或清除账号数据。
- 代表性 Build ID：`libgndownload.so` = `45a2263934179eae4cd7bc536e977519eeab61ae`；`libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；`libpulseaudio.so` = `ad97886e4ccdf2f02bec63d21c9733da0bfefe45`。完整列表在仓库外 `ayaneo/apk-audit-device-final.json`。
- 真实 Windows 用例：官方 [7-Zip 26.03 x64](https://www.7-zip.org/download.html) 的 `7zFM.exe`（PE32+ x86-64 GUI）。安装包 SHA `0859c524b8a63551848f0c246abddcb1d0b7b656b0fbfe879f8d85e61a9e6edd`，与发布者 GitHub asset digest 相符；主机仅解包；设备已执行 x64 → FEX 路径，见下节。

构建、测试、APK、组件和失败日志保存在仓库外的 `xrgame-native-evidence/wp3/20260926/`。Linux 完整构建日志位于数据盘根目录。PE strip、旧格式字体识别及中间测试失败记录保留。

## AYANEO Windows x64 实测

用户已授权直接使用替换后的 AYANEO，并亲自完成 Steam 登录后交回 UI。设备为 Pocket DS / Android 13 / API 33 / 4 KiB 页；完整 build fingerprint、boot ID、序列号保存在私有 `ayaneo/device-before.json`，不进入公开仓库。

- **实际执行**：官方 `7zFM.exe`（PE x86-64），应用 PID 24180，Windows 程序 PID 25403。APK 为 `6a88f1dffbbc914347069f9e2dca6367f149f76ce23ed2e6d1038022517c604d`。14:58:59 取样时 `/proc` 测得存活 **595.11 秒**（CLK_TCK=100）；这是观测时长，不是性能基准。
- **FEX 证据**：进程 maps 包含自建 `libarm64ecfex.so` 和 FEX JIT 映射。窗口正常显示，键盘可浏览 C 盘；在 7-Zip 中按 F7、输入名称并确认，实际创建 `C:\WP3-Input-Proof`，随后通过 app UID 下目录状态交叉核验。Alt+F4 正常退出，Wine 子进程消失。
- **最终 APK 复验**：应用 PID 28344、7-Zip PID 30616，观测 49.63 秒，成功打开同一 C 盘并保留先前创建的目录。PulseAudio 改为应用持有的前台子进程；Alt+F4 后 15:06:17 按应用 UID 检查仅剩 Android 主进程，**Wine/PulseAudio 残留为 0**。证据 `final-execution.json`、`final-after-exit.json`、`7zip-final.log` 和截图18。
- **内置运行时**：六归档共 141,786,075 bytes。`prepare-runtime-bundle.py` 检查默认版本、归属与 SHA；Gradle 再次验 SHA 后打包，安装时仍验 SHA。14:40:39 日志确认 base imagefs 和 Proton 从 APK assets 安装；其余依赖已按同一 catalog 安装并复核。篡改 Proton SHA 的打包前置负例被拒绝。
- **失败与修复**：旧 native wincomponents 请求在本仓 catalog 中不存在，改用 Wine builtin；容器准备结果增加落盘；argv 解析不再把外围引号传给 Wine；picoXr 的 evshim/DXVK 路径使用当前应用目录；关闭启动前上游配置请求、AI 调试推销及会员推销。
- **回归**：argv/组件相关单测 13 项通过，0 失败/错误/跳过；最终 APK 构建通过（37 秒）。前述 1370 项是设备调试前的全量测试，未冒充最终 APK 的全量重跑。
- **FEX 维护**：核对 shadPS4 与 XRGame 的 FEX 均为 `tencentmalos/FEX@3f1f30a0`，已创建本地 `feature/malos/xrgame-arm64ec`。尚无子仓提交/推送；gitlink 与 `.gitmodules` 的已发布分支未变。

私有证据包含 `7zip-attempt1..4` 失败/成功日志、90 秒录屏、`14-7zip-created-folder.png`、`attempt4-execution.json` 及进程 maps/stat。未引入闭源 redirect，未实现新的 WP4 shim；本结果不代替 Swan Android 16 行为矩阵。

## W3 管线续验（同日）

新增 [统一构建入口](../../tools/xrgame/README.md) 与手动触发的 `xrgame-runtime.yml`。入口冻结配方后启动 Ubuntu 24.04 容器，按顺序构建 Wine、FEX、D3D、Turnip、XCB、PulseAudio、组件包和 imagefs；源码记录阶段绑定六组件清单、归档与来源索引。workflow 权限为 `contents: read`，仅保存 runtime/source/evidence，没有 APK 上传或自动发布步骤。

### 空构建目录重放

数据盘 `/data00/xrgame-native/replay-20260926`：使用已有编译工具链和固定源码缓存，重新解包已验 SHA 的 Termux sysroot，`build/` 与 `output/` 初始为空。**八阶段全部通过，07:19:41Z–07:29:12Z，共 571 秒**。这轮没有复用此前编译输出；仍复用了已准备好的工具链，不能单独当作干净容器证明。

- 六组件 draft catalog SHA：`76fbfad059235152e9459af862a6e65f75175fda14d94ccbd934ad9bb07cb0c8`。
- 运行时源码归档 SHA：`4512207c402e79bc9be3c76438011040ac0a0381ce8106882e34745c42ff315b`；包含 29 个源码仓库/子仓、51,676 个文件/链接记录，覆盖本地补丁、配方、XCB 源码及 ntsync Rust vendor。未初始化、未用于本次构建的子模块显式列入 omitted 清单。
- 依赖源码归档 SHA：`afcc431bb1ed14f5d2c4fcc2616b7803e91e056c38fac96fe5d53eff697aa859`；覆盖 imagefs 的 81 个包，共 81 个经 SHA 校验的源码/配方对象。80 个包版本匹配同一 Termux 配方快照，libwebp 使用其 `1.6.0-rc1` 历史提交；无独立下载的源码包含在配方树内。
- 与上一轮逐文件身份记录比较：DXVK 0/5、FEX 0/4、Wine 67/1583、Turnip 0/1、VKD3D 0/2、imagefs 库 111/112 的二进制字节相同；无二进制文件增加/缺失。构建路径、工具环境和时间相关差异尚未全部归因，**不宣称位级可复现**。记录为 `pipeline/replay/logs/binary-comparison.json`。
- 这些重编字节尚未安装到设备；上一节 AYANEO 的运行结论仍只绑定其 APK 和组件 SHA。

### 源码与发布前检查

`termux-sources.lock.json` 固定来源 URL、SHA、版本、配方路径和 libwebp 覆盖项。源码下载传输失败可以使用登记的镜像，但内容 SHA 必须不变。实际下载中的直连失败日志也保留。

原始 Termux 预编译快照没有各包的构建提交证明；版本匹配不是原二进制来源证明。librav1e 的 Rust 构建依赖、ICU 替换许可文件、NDK 来源关联和完整源包重建仍需补齐。因此两个 source index 保持 `completeCorrespondingSource: false`，build record 保持 `releaseReady: false`。

`audit-runtime-release.py` 已对真实产物运行，按预期因上述前置项失败。**13 项管线测试通过（8 项发布检查、5 项归档检查）**：覆盖正常一致证据、缺源码、产物篡改、source index 变化、上游 URL、混入 APK、空组件及未完成构建记录。归档检查还覆盖内部哈希、路径越界、绝对链接及链接循环。记录在 `pipeline/pipeline-tests.log`；此检查是构建/发布前置校验，不是已经执行的 Release。

### 新容器完整重放

另开新 Ubuntu 镜像与构建根，重新安装工具链、拉取固定源码，未使用旧编译工具安装或对象文件。已记录并修复：Linux GitHub 直连卡住，改用临时代理；pip 引导阶段缺 PySocks，改用该代理的 HTTP 接口；Wine 宿主工具缺 FreeType，补入 Dockerfile 的 `libfreetype-dev`。失败日志保留。Git checkout 的中断重试及错误 pin/脏源码拒绝也已补入准备脚本。

修正环境后，数据盘 `clean-proxy-20260926` 的八阶段全部通过，**07:45:21Z–07:55:49Z，628 秒**（只计这轮构建/打包，不含下载准备和失败重试）。Docker 镜像 ID `sha256:2627765cce448567614ed76e34bd5dfd6292be42cecfc2427ade1733a6ef75d5`；工具链重新安装，源码从固定 Git 提交重新拉取。输入下载缓存均重验 SHA；未复用旧编译对象。

- 六组件 catalog SHA：`d1d262a58292e192434d1529fb952ad8652d2e7520230d61d0703102d0e20d35`。
- 运行时 source archive SHA：`612de05ba580e26359514a3b4937de15e8ea50e1a1af908846263ff551426a94`，29 个源码仓库/子仓，51,681 个文件/链接记录。
- 依赖 source archive SHA：`759b36ef92148ab02b10a5e72aaed56a8708cc06dc1170273fc6f48fbe4b83d3`，81 个包/81 个源码与配方对象，已绑定本轮 imagefs record。六归档的独立内容检查全部通过；发布前检查仍按预期因三项源码/发行前置条件失败。
- 完整逐文件 SHA、Build ID、宿主包版本与阶段日志在私有 `pipeline/clean/`。设备仍使用此前经过 UI 验证的组件，这轮编译结果没有设备执行结论。
- 新容器重放验证了固定脚本与依赖准备；GitHub Actions 尚未触发。最终 CI 还增加了源码下载/绑定和归档检查步骤，这些步骤已分别在本机/远端真实产物上验证，未冒充一次完整 GitHub Actions 运行。
- 本轮临时代理隧道已关闭，新建构建容器退出并移除，原有 `xrgame-wp3` 容器保留。未操作设备、提交、推送或发布。

## 游戏与调试续验

Hades II 后续已在 AYANEO 上通过 AHB GPU 拷贝路径进入实际场景、响应键盘移动并退出重启，
不再停在早期黑屏状态；当前 AYN 尚需复测。记录见 [Hades II](wp3-hades2-20260926.md)。native app 和 Wine x64 guest 的
基础调试链路已实测，复用方法与失败/清理证据见 [调试验证](wp3-debuggers-20260926.md)。

## 剩余项在新规划中的位置

1. **P1/P2，当前功能工作：** 在 AYN 重放已有 x64/GDI 与 Hades II D3D12 结果，补 D3D11、音频、物理手柄及运行/退出检查；处理 A 盘浏览、旧 E 盘路径和完整下载/续传/交叉校验。AYANEO 的成功不替代 AYN 结果。
2. **P5，最后可选：** 对应源码归档、完整依赖许可与来源归属复核、组件 Release、生产 catalog。所有 runtime 包仍为 `releaseReady: false`，不影响当前本地验证目录的使用。
3. **P5，最后可选：** 实际 GitHub Actions 运行及全量构建/测试收口。已有空目录和新容器重放证据保留；workflow 未提交/推送/触发，不宣称位级可复现。
4. **P4，延后：** Swan Android 16 行为矩阵与 XR；选择 OpenVR 功能后再接 OpenComposite。当前先测 AYN，再按测量决定是否增加 clean-room shim；新增组件进入本地构建，不等待公开发布。
