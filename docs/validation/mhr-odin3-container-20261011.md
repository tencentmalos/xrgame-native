# Odin3：本地 Container 更新与 MHR 复测

2026-10-11，`feature/malos/swan-xr-ahb`，远端基线 `cee4b8ee`。
按用户要求切分支并更新；保留原 Apple Container 环境的未提交改动及 stash 备份。
主干仍为 `malos/main`。本记录的构建与测试使用本地 Apple Container。

## 更新前

设备为 **AYN Odin3**，Android 15 / API 35、Adreno 830；不是此前验证过的 Thor。
MHR（1446780）进程 PID 18927 仍存活，截图为大面积彩色竖条/花屏。
这一状态不能描述为已经发生进程崩溃。

旧 APK SHA-256：`7a9942c113a49bbe1e343495471dce9f46bc405a2548df49092845b23e72a09c`。
组件清单：`e8f2780a71024d47b4f12ff1b74616354f588573d77ae105d8c93162337732bb`。
游戏选择 Turnip xrg5；旧 APK 不含 `libxrgame_wine_mapper.so`。
日志出现 Mapper namespace 加载失败。该现象与此前小米花屏有相似性，但仅凭这些
不能确认所有条纹均由同一个原因导致，或把驱动更新与 Mapper 修复的作用分开。

已在仓库外备份旧 APK、签名、容器设置、进程 maps、截图和日志。
MHR 留有 D3D12 API capture 配置；此次按正常游戏路径复测，使用已有
`api_replay.py mode --mode off` 清除录制配置。未清 shader cache、修改游戏文件或存档。

## 本地构建

- 应用：`picoXrDebug`，Android 原生库 `RelWithDebInfo`。
- 本地容器原有基础工具链可用；切分支后缺少对应的新运行时产物。
- Turnip：`adb7e30a` + 仓库中三个补丁，按 `build-turnip.sh` 构建 Wine X11/AHB
  和 Android 两个库；XCB 按固定 1.17.0 源码重新构建。
- 本地 Turnip xrg13 ZIP SHA-256：`99a669633b312d523c95e0e174276e6c3b79fe9c89b2faa60ee3bf373b122c03`。
- 新组件清单 SHA-256：`0096d42f1871b3c97961d8c34e1c17ab91bd86a3d69dd1e8e8f91caf1eaeabd3`。
  其余六类组件仍使用原暂存且已验证哈希的制品，没有重编全部 Proton/FEX。
- OpenComposite：沿用原 pin 的 OpenOVR、四个补丁和依赖。使用 LLVM 21.1.0
  的 Clang MSVC ABI、微软 SDK 10.0.26100、CRT/ATL 14.44.17.14，`Release /MD`。
  DLL SHA-256：`f5a979e0194eb567410587e3d05400f298ac6a54bc2223f8d26dcb0a995eb31d`。
  PE x64、imports/exports 与 OpenVR ABI 编译检查通过；没有改写 OpenComposite 运行逻辑。
  原 MSVC 制品及其验证记录保留在 pin 的 `previousValidatedBuild` 中。
- 普通 MinGW 试编因缺少 ATL 未采用；所有失败日志保留。最后只补齐 SDK 文件大小写别名，
  构建源码仍为原 pin + 四个补丁。**此本地 OpenComposite 制品尚未验证 VR 游戏**。

容器 DNS 随当前 Mac 网络刷新；源码镜像增加同步新分支使用的 Vulkan-Headers 和 fmt。
细节见 [本地构建说明](../building/apple-container.md)。

## 设备身份与证据

- Build：`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`。
- Boot ID：`c322cd23-e6c8-46f8-8eb7-ff032d7162db`。
- Debug 证书 SHA-256：`ce9d5b4d6524b793e48e00658f4d6c9d2ccfa1b1059dbb1fb34244c68cd1f8eb`，与旧 APK 一致。
- 私有原始证据：仓库外 `../xrgame-native-evidence/mhr-odin3-20261011/`。
- 构建产物和源码/工具链记录：`~/Library/Developer/SteamPSP/linux/` 下
  `artifacts/20261011T042633Z/` 与 `runtime-candidate-20261011/`。

## 更新后结果

APK SHA-256：`2b6d593149389c07fde5e27af712bd91fd89165bd8a7d224fa544b0a579d8c0f`。
设备安装包回读哈希一致；保留数据安装成功，APK 审计 126 项、0 错误。
应用 PID 24244，MHR PID 24653。运行中 maps 含 Mapper 适配库，容器自动选择 xrg13；
实际加载的 Turnip SHA-256 为 `5d05a560c994d5c6b1903636379341035720827280efc049546e4dbddbc12713`，与本地包一致。
已正常显示启动图和 `Compiling shaders...` 进度条，原彩色竖条消失。
编译结束后进入 3D 猎人创建；后续画面继续到改发型、输入角色名，均不是助手发送的按键。
因此只做后台观察，没有代替用户创建角色、输入名字或改动存档。
同一 MHR PID、同一进程 startTimeTicks 在两端采样间保持一致，观察跨度
**519.31 秒**；其中角色创建阶段采样跨度 **127.68 秒**。
本次日志没有观察到游戏 fatal signal、Java fatal exception 或既有 `0x50000018/deadc0de` 签名。
截图 `title-menu.png` 实际是角色创建，`character-check.png` 是角色名输入；以画面内容判定状态。

这证明本机更新后的启动与 3D 捏人显示正常，**未验证原存档、实际关卡、长玩或 VR**。
没有做旧驱动/Mapper/录制配置的逐项 A/B，因此不将改善唯一归因于某一项。
测试结束停止了本轮 logcat 与 DebugBus 服务，游戏保留在设备当前界面。
此次没有手动清缓存，驱动更新后出现 shader 编译不能描述为游戏卡死。

| ELF | Build ID |
| --- | --- |
| `libvulkan_renderer.so` | `8ca0ab5ad6f110643c997a962cf805a182b82c37` |
| `libxrgame_debugbus.so` | `25668681825bab0a12cde188c0b1eccdacb565ef` |
| `libxrgame_wine_exec.so` | `d3249af2c39a46757c14457ea207817ac6f6385a` |
| `libxrgame_wine_mapper.so` | `294c1d545d7b0b35291b1b6a22eff39a8af424fe` |
| `libxrimmersive.so` | `d20eff09566ab8509140b146ee7b0aa93a6f4e62` |
| `libvulkan_freedreno.so` | `29955445096b518b43180b0b49c92b9b27ed67ed` |
| `libvulkan_freedreno_android.so` | `c1517f7723269d3f4d7025085e31bde0f3b77811` |
