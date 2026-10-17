# 官方 Linux Steam 接入：2026-10-16

分支 `feature/linux_steam_support`，基于 `malos/main` 的 `78eb268a`。用户要求切到官方 Linux Steam，其他运行实现尽量保持；缺失主干制品在本机 Apple Container 重编。当前内部候选已在 AYN 登录官方客户端并通过 Bionic 会话检查，尚未完成游戏联机验收。

## 边界

- Linux Steam 桌面客户端：ARM64 glibc userspace，通过源代码构建的 PRoot 运行，targetSdk 维持 36。
- Windows 游戏：保留 Bionic Proton 11、FEX、ARM64EC DXVK/VKD3D、Turnip、现有 X server/AHB、音频、手柄与 XR。PRoot 不包裹游戏进程。
- 客户端单独使用 Xephyr 的 X11/GLX/font/Render 和软件 Mesa/CEF，最终图像送往现有 Android X server；键盘复用 Android IME。此显示策略不改变游戏。
- Steam ABI：优先使用 Valve 官方 `androidarm64/libsteamclient.so`，通过现有 Wine `lsteamclient` 转接。只修正它原先硬编码为 `app.gamenative` 的库路径。当前未采用额外 RPC Steam API 代理。新的 Wine 候选只替换 source-built `lsteamclient.so`，其余归档成员不变，仍通过正常 runtime receipt 校验；不在安装后偷偷覆盖已校验的 Wine 文件。
- 安装器只从 Valve HTTPS CDN 下载，校验每个归档大小和 SHA-256；拒绝路径逃逸。未实现 Valve manifest 签名校验，不把 HTTPS+SHA 宣称为签名验证。Valve 二进制不进仓库和 APK；登录由用户在官方界面完成。
- 当前 JavaSteam 游戏库/安装/云同步保持原实现。不能把客户端页面接通描述成已经删除 JavaSteam；游戏启动代码已切为官方会话前置检查，并要求它与现有库/云同步账号一致；无会话时明确报错，不静默退回 GBE。官方登录及 Bionic 会话探针已通过（见下文 v14/v15）；游戏端验收仍未完成。

## 已测前置条件

独立探针包 `com.tencentmalos.linuxsteamprobe`，不是 SteamPSP 游戏验收包：

| 项目 | 结果 |
|---|---|
| targetSdk | 36 |
| 执行域 | `untrusted_app`；未降 target、未依赖 root/run-as 执行 |
| PRoot + glibc | glibc 2.39；pthread TLS、Unix socketpair、fork/exec 通过 |
| 官方 Bionic 库 | `dlopen` 和 10 项接口导出通过；尚未调用登录/票据接口 |
| 探针总耗时 | 244 ms（单次） |
| 探针 PID | App 8249，glibc 8289，Bionic 8302 |

设备为 AYN Thor，Android 13 / API 33，ARM64，4 KiB。Fingerprint：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；boot id `b509848d-21a5-4f1d-b6ed-24c0572aa9d2`。序列号及原始日志在私有制品目录。

| 制品 | SHA-256 / Build ID |
|---|---|
| 探针 APK v2 | `9d9354c32f77e13c9ac9ec31da425d0b76cd5c5b64a37d9a6bb215f41dd05c4a` |
| PRoot component manifest | `e5fc20a709c6d6cd950d824e68035fd3b261a7ef5149e359dccedf8c07a70190` |
| PRoot Build ID | `a6b8f3798a5dc7c927a86f58a742e373dc21c6a0` |
| PRoot loader | 无 Build ID；SHA `6f2ef63ededcac31733d21e956d5d0cd27de0b9e2475ae36548bc371f038a4b8` |
| Bionic load probe Build ID | `789a52b1d094ad3f2c83e2013cf12e4cf0e0a692` |
| 官方 libsteamclient.so | SHA `de56d4bc889a5b42ca8018fcf53e6cba260647b9a48e448f3d77e1d4625725d9`；Build ID `244c71f0592ee16eebaed7c3a3c628900f2a3368` |
| Wine lsteamclient 候选 | SHA `fb9d78a8e51e447fab9030013e69991d43e06619674bc0b8a25a4f2a9d31979d`；Build ID `de3ef079ca0844d41c90ca1ad7614eab4e864129` |

Wine 适配库从原运行时 pin `5d0d333e` 源代码编译，只有 `wine-linux-steam-client.patch` 的库路径变动；成功编译不等于会话或游戏验收。

## 本机重编主干组件

Apple Container Linux x86_64 / NDK r27d / bylaws LLVM-MinGW 20250920；OpenComposite 使用相同 LLVM 的 clang-cl MSVC ABI 与已配置 xwin SDK。未降级主干版本。

| 组件 | 新归档或 DLL SHA-256 |
|---|---|
| Turnip xrg16 | `b3df0d95a1a84e23090d52f3e1f124d6faa85ab00176457b0aa3b974367066ee` |
| DXVK xrg7 | `1e9efcabd1af0e78a057c6bdc7eaaad37859fbd37941a159015fcf56ff0b36c8` |
| VKD3D xrg4 | `feb565db0f602f53f1c3d5b7e25218c1d5603ff04a421f71b9e8397f21963820` |
| OpenComposite `20de29d9` 配方 | `ce6bafc13b81313c0cd541a4f41215ee3e7c337c7679f5ca1107db70bb2fbc70` |

上述是新编译候选，不能继承旧二进制的 Swan 设备验收。OpenComposite pin 保留旧 Windows/MSVC 的 hash 与证据。早期失败日志保留：PRoot 构建工具/flags、Wine 补丁头和 X11 头文件、安装器归档内部链接、首次主 APK Kotlin 编译错误。

## 2026-10-17 实机增量（未完成验收）

- APK v3 `4ce855153fd078f107d7796c2d91d66ea489ba6676eb1164023c7dfff7bc7da7`、v4 `2ac624e938dc20f1eaa7227772c4a03086853b7aacc9b846941c209c50b1c5f1` 均由本机 Apple Container 构建，native 为 RelWithDebInfo；audit 各 139 项、0 errors。
- v3 在 AYN App PID 32656 下运行；客户端更新停在约 61 MiB。X server 未识别 `PolyText16` 后没有消费正文，误把正文当作后续 `CreateWindow`，产生 BadWindow/BadIDChoice。
- v4 修正未知请求的边界处理（消费正文、返回 BadRequest），并修正专用 X server 关闭 MIT-SHM 时的 signed opcode key。JUnit wire regression 已通过：未知请求后的真实 NoOperation 不会被正文伪造的窗口请求干扰。
- v4 App PID 23622、PRoot 24680、Steam 24686/24687：00:12:42–00:15:21 UTC 观察 159 s，Valve 自身更新完整下载、解包并报告 `Update complete, launching Steam`。这不等于登录页已可用。
- 随后窗口销毁触发 `DrawableManager.removeDrawable` 的空 renderer 崩溃：独立页面漏了 `XServer.setRenderer`。候选修复注册 renderer、页面销毁解绑 listener，并允许无显示页面时回收窗口；待下一版实测。
- 更新器的 `PolyText16` 字体绘制仍未实现，v4 更新窗只有灰色背景；不能称 UI 验收通过。原始失败/崩溃日志和截图保存在私有 `linux-steam-device/run1`、`run2`。
- 新 Proton 候选 `proton-11.0-2-arm64ec-linux-steam1.wcp` SHA `476769081f7281a72e107403bcc9ff9a17ac68180c01e05e5f9de2ae1f1f860d`，只替换 `lsteamclient.so`；strip 后 SHA `e6a76b6be8856caea7c84974d25dc5380056ea514b8963046ef71a7100b29c75`，Build ID 保持 `de3ef079ca0844d41c90ca1ad7614eab4e864129`。基线是本机已有 xrg3，不宣称合入主干后续 xrg6/xrg7 的 Wine 改动。
- Python bootstrap 11 项测试通过，覆盖 Valve URL/redirect allowlist、manifest/架构筛选、归档路径/内部链接/解包上限、保留已有客户端、只提取官方 Windows ABI DLL，排除 Windows 桌面程序。

### v5：窗口生命周期修复与下一层阻塞

- APK `4d31a692f945c4a06aed1ac2caebab7d6d4d421b5791a17728765d697f16269e`，audit 139 项、0 errors，RelWithDebInfo。Runtime catalog SHA `09af55aa38f74683a3f0fa11035a236f63a6ca8225f8630c285e6d5badb9f91a`；Linux Steam component manifest SHA `487cc6367821dd36dbbb1738bddd9808049e756ef6dbefd24c15474cab09e266`。
- PRoot Build ID `a6b8f3798a5dc7c927a86f58a742e373dc21c6a0`；Bionic session probe `df3973dbcaac56ed724ddc4a281469845d6e12af`；DebugBus `9adeb5677886e4b87963d0a4553e69c47628a909`。完整原生 Build IDs 在私有 APK 审计记录。
- 同一 AYN/boot，App PID 31790，PRoot 348，Steam 354/355；00:26:30–00:26:33 UTC 的 3 s 启动中，更新器窗口销毁不再触发 App Java 崩溃，服务正常报告客户端退出 255。此时客户端已更新至版本 1792192922。
- 实际退出原因是 `steamui.so` 缺少 `libpipewire-0.3.so.0`。ARM64 Ubuntu 下对 Steam、steamui、steamclient 和 steamwebhelper 做依赖闭包检查，发现这一缺项。原始日志/截图保存在 `linux-steam-device/run3`。
- 仅为继续定位，在停止客户端后向其独立 userspace 暂存 Ubuntu 的 PipeWire 库（不是游戏文件，也不是最终验收包）。00:28:09–00:28:14 UTC 的 5 s 复测越过动态加载，随后 `XSyncInitialize` 收到 BadImplementation（opcode 152 / minor 0）并退出。日志保存在 `run4`；该临时库将在新 userspace 整体更新时替换。
- 候选修复把 PipeWire 加入 userspace 配方，并仅在客户端专用 X server 中关闭未完整实现的 SYNC、DRI3、Present；游戏 X server 保持不变。userspace 升级先解包/校验，再替换系统目录；官方客户端 HOME、账号文件和机器标识保留。上述候选待 v6 实测。
- `PolyText16` 尚未实现文字绘制。当前修复只保证未实现请求不会破坏后续协议边界，不构成文本绘制完成。

### v6：userspace 升级通过，登录窗口仍未出现

- APK `4f1c7279d0d3e29f6899ad15171e07c78fa79dd2990d411dcfdc2518ccfa9500`；audit 139 项、0 errors，RelWithDebInfo。Catalog SHA `c389df3e8db71bf054362b188abdb2ecdf92454649eb94b24af8451fd9598943`；客户端 component manifest SHA `62c5a45e27876d3714b7dd673e378dcc2c7b3f036c8fbf9f85752bb95e4b81c5`。
- 同设备/boot，App PID 8378，PRoot 9636，Steam 9644/9647。userspace 已更新为 `e2fc4b6e3cfed23a82261c0c465ce2733323b482a306ab57546083b655c2ac94`；升级前后 machine-id 与已更新的官方 Steam 程序 SHA 均相同，没有重新下载客户端。
- Wine 适配库按显式 xrg3 输入重新构建：候选归档 SHA `3ff788f90c3394f82b4cd28df1b64ef098e80719a940bae7afda9864c8c845ea`；`lsteamclient.so` SHA `0ac05ae0c5988aa8a6b6824565b5f17413289ebabc1ac9f80e1efdcc3b63f71a`，Build ID `181bb2d406cf867d5a28e8d68ec72eb9752f5826`。代码改动范围仍只有原来的库路径补丁。
- 00:42:25–00:42:31 UTC 完成校验并关闭更新窗口，没有此前的 PipeWire/SYNC 错误。之后主线程停在 `ppoll`，尚无 CEF 进程或登录页；原始记录在 `run5`。Bionic 探针仍为 5/no-pipe，不能据此断言两套客户端 ABI 不兼容，因为桌面会话初始化尚未完成。
- 额外对所有 ARM64 glibc 模块查依赖，发现此前漏查 `steamrtarm64/steamclient.so`（之前只查了 `linuxarm64/steamclient.so`），以及 VGUI/音频模块。补入 OpenAL、libnm、SM/ICE、VDPAU、SDL2、libssh2；在独立 userspace 暂存这些 Ubuntu 库后仍停在同一初始化阶段（`run6`），配方已同步补齐，待新资产完整重打包。不能把这些临时增补当作 v6 APK 内已包含。
- 另确认 `GrabServer/UngrabServer` 的实现错误地发出了回复；本机固定 xcb-proto 的 `xproto.xml` 明确将 opcode 36/37 定义为无回复请求。候选删除额外回复、增加 wire regression，并提供客户端专用 64 条请求环；尚待 v7 测试。环只记录连接 ID、序号、opcode、data、长度和完成标记，不含请求正文、输入文本或登录信息，游戏服务器默认不启用。

### v7：定位 QueryKeymap 未刷新回复

- APK `d696166516fde08613e73c94d25720e72ca821b8803d8a4b1c54ddaeaa86dcaf`，audit 139/0、RelWithDebInfo；component manifest/catalog 与 v6 相同，设备仍有 `run6` 的临时依赖增补。App PID 14353，PRoot 15216，Steam 15280。
- 两项 wire regression 通过：未知请求正文不会变成伪造的后续请求；GrabServer/UngrabServer 不插入额外回复。
- `linux_steam trace` 显示 Steam 连接 `8388608` 的序号 80、opcode 44（QueryKeymap）已执行完成，之后没有新请求；主线程停在 poll，未生成 CEF。对应实现只写了回复 buffer，漏掉会在 close 时 flush 的 `XOutputStream.lock()`。
- 已补输出锁和 40 字节回复刷新测试，待 v8 安装验证。这里定位的是初始化等待，仍未完成登录页验收；不能把 GrabServer 修复单独宣称为黑屏已解决。
- 试验性 Android 点击和自有根窗口属性 `_STEAMPSP_PROTOCOL_WAKE` 的变化均未推进客户端；该临时属性已删除。证据在私有 `run7`。

### v8：QueryKeymap 通过，继续修正 Atom 查询

- APK `fb1d251f890eeb9dac5a3c310cb97056631ab3ec549a2b1b4544f5d66806ddb6`，audit 139/0、RelWithDebInfo；客户端 component manifest `5702c24b85e1663790fe1a5fb49883561cbd68e66112a6877aa6c0cb623315bf`，userspace `c43493c85e6d50162d81465d3fffefbf179a638ee9fb92446f5175ef1e4322e3`。Catalog 同 v6。所补 Ubuntu 依赖已正式进入资产，安装时完整替换了临时诊断 userspace。
- 三项 X11 wire tests 通过（新增 QueryKeymap 完整回复及 flush 检查）。同设备/boot，App 18462，PRoot 19453，Steam 19459/19463。
- 01:00:47 UTC 越过此前 QueryKeymap 等待，随后另一个 X 连接在序号 7 的 InternAtom 请求收到 BadAtom 而退出；trace 为 opcode 16、OnlyIfExists=1。现有实现将缺失名字的内部 `-1` 当作错误，而协议要求返回 Atom None/0。
- 候选改为返回 None，同时将 GetAtomName 的非法 ID 转成 BadAtom，避免数组越界或空指针；增加两项 wire tests，待 v9。原始日志和 trace 保存在 `run8`。GLX/RANDR 不支持的提示也保留，当前没有把它们认定为此轮退出根因。

### v9：Atom 查询通过，定位颜色协议缺项

- APK `7c17f0aeb7cdd536a405e48eb734f4ed0722937ae990f2dcd00af601a86262f8`，audit 139/0、RelWithDebInfo；catalog 与 v6 相同，客户端 component manifest 与 v8 相同，原生 Build IDs 没有变化。五项 X11 wire tests 通过。
- 同设备/boot，App PID 22213；01:07:00–01:07:06 UTC 的日志显示更新器校验和窗口销毁完成。随后连接 `16777216` 已通过两次 `InternAtom(OnlyIfExists)`，但在序号 9、opcode 85 的 `AllocNamedColor` 收到 BadRequest 后退出。原始日志和 trace 位于私有 `run9`。
- 候选为现有 TrueColor visual 增加 colormap 资源、颜色分配/查询/释放和一组常用 X11 名称，未知名称仍返回 BadName；默认 colormap 不再报告为 None。协议布局对照固定 xcb-proto `xproto.xml` 和 [X11 core protocol](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html)。这不包含文字绘制，`PolyText16` 仍未实现；待 v10 验证。

### v10：颜色协议通过，确认 VGUI 必需 GLX

- APK `bad0fdecd8a0a91d6ad80221861d7fe74dfb0c87746c870b8facd54b89796b6f`，audit 139/0、RelWithDebInfo；catalog/component manifest 与 v9 相同。八项 wire tests 通过，新增名称大小写、TrueColor 分量及 pixel 布局、AllocColor→QueryColors 一致性和 BadName/BadColor 返回。
- 同设备/boot，App PID 28496、Steam 29427；01:19:48–01:19:54 UTC 启动，客户端已越过 AllocNamedColor。随后 `surface_linux.cpp:1994 glXChooseVisual failed` 触发 fatal assert。现在有直接证据确认 GLX 是 VGUI 的硬要求；仅 `-cef-disable-gpu` 不够。
- 下一候选只在官方客户端 userspace 中使用 Xephyr + Mesa llvmpipe，提供完整 X11/GLX/font/Render；现有 Android X server 承载其最终图像和输入。Windows 游戏不经过 Xephyr 或 llvmpipe，仍保留既有 Wine/FEX/AHB 路径。此客户端软件路径会增加 CPU 拷贝与渲染开销，尚未测量。不能将它描述为已实现原精简 X server 的 PolyText16。
- 原始失败日志、trace、截图保存在私有 `run10`；尚无登录页。

### v11：Xephyr 客户端显示候选

- APK `ed7a1373272e0db9250608689c7942f4d37fec11ddf0ec9f548b4036d86fabd0`，audit 140/0、RelWithDebInfo；catalog 同 v6，客户端 manifest `7b53421cd23c8a5383e062aa7d3d1f5e766a2e941a2e921d50d45c9dee20a280`，userspace `669b5df8407dfde607ced155e7de72acfeb1bb8e1f6bf61df46e066a256ceab8`。新增启动器三项测试通过：保留更新退出码 42、启动失败不拉起 Steam、中途停止回收显示进程。
- 同设备/boot，App PID 452，01:28:25 UTC 采样：Xephyr 已通过宿主 `LookupColor/AllocColor`、窗口和光标创建，但报告 `Unsupported screen depth 32` 后退出，监督进程返回 70。此轮未运行到 Steam；没有 GLX/文字显示验收。
- 下一候选只将客户端专用 X server 的根 visual 设为 24 位 RGB（像素存储仍 32 位），既有游戏构造参数默认维持 32。原始失败日志、trace 和截图保留在私有 `run11`。

### v12 / run13：官方登录页可见，仍未登录

- v12 APK `fc409156ad7613896048742a03c1d97e1604af1ea311b4170b019dd3689d6ba2`，audit 140/0、RelWithDebInfo；客户端 manifest/userspace 与 v11 相同，catalog 同 v6。App PID 6300、PRoot 6646、Xephyr 6923、Steam 7289、CEF 7379。24 位宿主显示已让 Xephyr 完成启动，Steam 越过 GLX 断言并生成 CEF。
- 01:35:15–01:35:49 UTC，随后 Steam 因 `Failed to connect to websocket` 退出；反复提示 `lsof: not found`。这次并非 GLX 或字体失败。保留在 `run12`。
- DroidDeck 的 `preload/net.c` 记录真实本机 socket 归属，供 Steam 的 lsof IPC 校验查询使用。Shell 在调用 exec 前仍需找到 lsof 文件；userspace 漏装了它。停止客户端后只向独立 userspace 暂存 Ubuntu ARM64 的 `/usr/bin/lsof`，复测记录为 `run13`；这个增补不是 v12 APK 自带的内容。
- run13 App 6300、PRoot 8816、Xephyr 8823、Steam 8829、CEF 8936，01:38:29 UTC 截图看到官方账号/密码登录页，01:39:43 UTC 二维码也生成。没有输入真实账号或密码，没有提交登录。
- Bionic 会话探针从 no-pipe/5 变为 not-signed-in/7：创建 pipe 和连接全局 user 已成功，不能据此声称在线、账号匹配或游戏端已验收。
- Android Gboard 实际显示于 AYN 下屏（逻辑 display 4），上屏截图看不到。最初按键没有到 Steam：原 DesktopHelper 只为携带 Wine WM_HINTS window group 的窗口设焦点，Xephyr 不符合。用一次受控 `XSetInputFocus` 给唯一客户端窗口设焦点后，下屏点击一个测试字母，上屏密码框出现一个字符占位；随后用下屏退格删除，未提交登录。该诊断不修改游戏或账号文件。
- 候选在客户端专用 X server 的窗口映射回调中设置焦点；不修改游戏的焦点规则。v13 已正式打包 lsof（APK `d423069ae6157bf276270913d59fdf7617e365e6e3104561615bf6151a1fe65e`、manifest `53a230da7874ac38798b2782cd229e260c80d3fc1791d4c83246da7f172fd236`），未安装；等待同时包含焦点修复的 v14。

## 后续验收

1. SteamPSP 的独立官方 Steam 窗口能显示登录页，Android 键盘可用。
2. 用户登录专用测试账号，Bionic 会话探针证实连接官方客户端；不得以导出存在代替在线状态。
3. 同一测试账号启动既有游戏，证明其 Steamworks 不再落到 GBE；验证已购 DLC、保存路径、退出/自重启、断网重连。
4. 比较普通游戏的显示/输入/音频路径与基线；本任务未采用 DroidDeck 的 gamescope、整套 Proton、GPU thunk 或输入音频实现。
5. 记录最终 APK SHA、各新 .so Build ID、component manifest SHA、设备/PID/时长。

## 2026-10-17：入口、大屏和系统语言（进行中）

- 用户要求增加 `LaunchActivity`，选择 Official Steam / Custom Steam。前者默认 Steam Big Picture（`-gamepadui`），后者保留现有 GameNative 游戏库和设置。两个既有桌面图标 alias 转向新入口，MainActivity 的游戏/下载深链仍保持原目的地。切换界面不退出官方会话。
- [Valve 的 Big Picture 说明](https://help.steampowered.com/en/faqs/view/3725-76D3-3F31-FB63)确认该模式用于电视/手柄；启动参数也对照 pinned DroidDeck 的 `droiddeck-session`。这里未接入 DroidDeck 的游戏运行时或 gamescope。
- Steam 使用 Android 系统的有序 locale 列表，映射到 [Valve 的语言标识](https://partner.steamgames.com/doc/store/localization/languages?language=english)，通过 `-language` 指定；简繁体优先看 Hans/Hant，再看区域。未支持语言按下一系统语言或 English 回退。Custom Steam 保留其原有语言设置（默认跟随系统），不覆盖已有显式偏好。
- 原 userspace 仅有 DejaVu，`fc-list :lang=zh-cn` 为空。新增 Ubuntu `fonts-noto-cjk` 和字体缓存，分别匹配 Noto Sans CJK SC/TC。Fontconfig 不接受 Android 的 `zh-Hans-CN`/`zh-Hant-HK`，转换为 `zh-cn`/`zh-tw`；不将缺失 glibc locale 写入 LANG，保留有效的 `C.UTF-8`。这是字体显示支持，不代表中文 IME 文本提交已接通。
- Java `Drawable` 的四个 JNI 声明漏了 native 已有的 `needsSwapRB` 参数，会从未定义寄存器/栈值决定是否交换红蓝。现补齐签名，并按每个 drawable 的 CPU AHB/普通 X11 存储决定转换，避免客户端局部刷新颜色随机变化。新增 ABI regression 核对七个 JNI 入口；不改变 D3D/AHB GPU Present 链路。
- v14 APK `e35487e7cd7dea93347e9240eff1092517b15525d4dd5825d72c7236ed0bdd76` 已构建、未安装，后续版本取代；不能将其视为已上机的焦点修复。
- 用户已自行完成官方客户端登录。09:52 本地时间按用户要求截屏，v12 + run13 临时 lsof/焦点环境显示官方游戏库；截图仅在私有制品目录。随后 DebugBus Bionic 会话检查返回 0（已登录且拿到有效 Steam ID，未在记录中输出身份）。未做与 JavaSteam 账号的一致性验证，未启动游戏，不能据此宣称 Steamworks/联机验收完成。
- v15 APK `fac5246c1fab0d099f4f464c97636672636a6cc4baba47bd46da668f7310d4b1`，audit 140/0、RelWithDebInfo。客户端 manifest `a31e7c8cf4d34e74bf6e85d2bf8337e206d4d0668b12074da8ca9eccd15abb50`，userspace `4f508e69e25286ea669d08e8c53f4cdd709996e6aff6d7a1007f457db48570cf`；catalog 同 v6，原生 Build IDs 与 v12 相同。AYN 同 boot；App 20371、PRoot 20657、Xephyr 20666、Steam 20672、首个 CEF 20713。10:03–10:04 本地时间已看到中文入口页、中文 Big Picture 首页/游戏详情，注入 Android BUTTON_A/B 能进入/返回，Bionic check 仍为 0；未再手动调用 XSetInputFocus。Custom Steam 返回现有中文游戏库，未启动游戏或修改安装目录。
- 用户复核指出 Android 状态栏及测试按钮不应出现在正式大屏界面。此轮 v15 的中文/导航证据保留，**不作为最终全屏验收**。下一候选删除全部 Android 工具栏和常驻状态文字，使用沉浸式系统栏；按设备真实屏幕比例给专用 X server/Xephyr 同步分辨率（AYN 为 1280×720），触摸坐标也使用实际 guest 尺寸。Android 返回离开 Official 页面，Steam 的 B 键仍只在客户端内返回。


## 2026-10-17：全屏键盘、中文与入口修复

本轮优先处理用户无法输入的阻塞。v17 去除 Android 标题和测试工具栏后，也丢失了唯一可见的键盘入口；X11/CEF 输入框不会自动请求 Android IME。修复给 Official Steam 页面增加右侧键盘按钮，保留全屏；Select/F6 和双指触摸也接到同一入口。输入前先选中 Steam 内的文本框，AYN 的系统键盘显示在下屏（逻辑 display 4），Steam 在上屏（display 0）。

实现边界：

- 继续使用 `IMEInputReceiver`，为客户端增加整段文本回调；游戏原有按键映射是默认行为。拼音 composing 暂存在输入连接，选词后才发送，过期连接不能继续发送。
- Official 页面声明普通文本编辑器，禁止个性化学习；原先的 `VISIBLE_PASSWORD` 会使 Gboard 即使选了中文拼音也只提供 Latin 布局。它不是 Steam DOM 输入框类型的探测器，不能宣称实现自动焦点/自动唤起。
- ASCII 经现有 Android X11 按键路径发送；中文通过有界、临时 X11 CLIPBOARD owner 提供 UTF-8，再由相同 Android X11 路径派发 Ctrl+V。输入只经 stdin 和内存，不写文本文件、logcat 或 Android 系统剪贴板。它不是系统剪贴板同步功能。
- 串行队列维持文本、退格、回车顺序；暂停页面取消队列并终止正在运行的 helper。Xephyr 内 XTEST 键盘没有可靠到达 Steam 焦点，失败探针保留，没有把此路径继续用于产品。
- LaunchActivity 改成两个图标背景卡片，保留 Official/Custom 的原入口语义。启动遮罩等待 CEF 主窗口或登录窗口，避免把更新器的中间画面直接显示为大屏首页。
- Noto Fontconfig 匹配已经正常，但原 Xephyr 只有 6 个西文字体条目。userspace 加入 `xfonts-base` 和 `xfonts-wqy` 后查询得到 651 条，包含 WenQuanYi `iso10646-1`；许可证留在 userspace 内并补入 THIRD_PARTY_NOTICES。启动瞬间原弹窗的每个中文字形尚未单独截图验收，遮罩不等于字体绘制验收。

### 构建和实测证据

| 候选 | APK SHA-256 | 状态 |
|---|---|---|
| v17 | `50c2a2274d89998cfb25f9229b975872cc5d2c1884babe64b0fd5e038f6aaec1` | 旧全屏页，没有可见键盘入口 |
| v20 | `abc99e1b8dcc1ec359b979900f6de1cb2d1f744d3b15e7991fdda71de20b3a09` | 保留数据安装，Gboard 可弹出，实际点击软键输入 `hello`；中文布局仍被密码类型限制 |
| v21 | `ffb728387a51d2a2860a46c1cb8dd3d54f368471d8facddbdf831fb32abf60c5` | 普通文本类型，实际 Gboard 拼音两次选词得到 `测试中文`，再次聚焦输入框后退格得到 `测试` |
| v22 | `a332d11c45eeca2df56fb61326054eeba873eb12b2b3ce1e0a507196ca9ee217` | 只完成构建/审计，未装；缩放键盘图标并避免按钮抢编辑器焦点 |

v20–v22 均为 Apple Container 构建、native `RelWithDebInfo`，APK 审计 141 项、0 errors。客户端 component manifest SHA `cc3f6a2bf1d1186c0e11c9b77fbd23502d7dc426f8c5c60bcb4cb58831dade16`，rootfs SHA `0b7f2a738eaa4d172817ad133dd020d9fa315da1c1def99b8cd3a6995f4bd83a`。Runtime catalog 同 v6。`libwinlator_11.so` Build ID `d57c4d4612088a6e48212ad7c177b3a8294079b5`，PRoot、DebugBus、session probe IDs 同 v5；全部文件和 Build IDs 见对应私有 `apk-audit.json`。

设备仍为本记录中的 AYN/Android 13/同 boot。v20 App PID 12035，10:55–10:58 本地时间完成键盘按钮和 ASCII 检查；v21 App 15599、Xephyr 16091、Steam 16097、首个 CEF 16174，11:01–11:06 观察约 5 分钟。截图、测试日志和失败探针在私有 `linux-steam-device/run15`、`run16`；记录中没有用户身份或认证数据。仅测试 Steam 搜索框，没有启动/下载游戏或更改存档。

v21 的 Kotlin 定向测试 23 项通过（IME 8、X11 wire 8、语言 5、显示尺寸 2）；Python launcher/bootstrap/JNI contract 共 17 项通过。v18 因仓库禁用 Toast 的同名类导致编译失败，改用输入失败对话框；v19 在修正 XTEST 路线时主动中止，未安装。失败/中止日志保留。

v23 APK `32210dcb43732f0f4cf0bea93d9fb78bab2f440bb1c4df5355885be0cfb0355f` 保留数据安装，仍为 RelWithDebInfo、审计 141/0；组件 manifest/rootfs/catalog 及原生 Build IDs 同上。24 项定向 Kotlin 测试通过。AYN 同 boot，App PID 22136、Xephyr 22460、Steam 22466、首个 CEF 22534；11:13–11:21 本地时间观察约 8 分钟，Bionic 会话检查为 0。右侧按钮成功重新弹出 Gboard，真实拼音候选“测试”提交到 Steam 搜索；清空搜索后 Home→桌面 alias 返回，键盘仍可再次打开。截图保存在私有 `run17`。

v23 的 Select 复测发现：按下事件在 Activity 打开键盘，紧接着松开事件经 `onKeyPreIme` 将它关闭。候选改为仅当同一个键的按下已被输入接收器消费时，才在松开时关闭；新增这一顺序的回归测试。v23 的快捷键重新打开不算通过，按钮/文字输入证据保留。

### 与 DroidDeck 的本轮对照及未完成项

固定 reference 的 KeyboardHost 同样依赖显式显示键盘；它也使用 `VISIBLE_PASSWORD` 和按键回退，不能直接作为中文拼音正确性的证据。`SessionClipboard.kt` / `tools/linuxfs/clipboard/clipboard.c` 的 UTF8_STRING、TEXT、仅 ASCII 时提供 STRING 的处理提供了协议对照；我们没有接入其持续监听系统剪贴板或共享文本文件。

刷新问题仍未关闭。v17 的一次 16.31 s 导航采样中，SurfaceFlinger 收到 248 个新呈现，约 15.2 Hz，帧间隔中位数 66.89 ms、P95 83.62 ms；屏幕是 60 Hz。场景控制不足，这只是问题基线，不是性能验收或确定瓶颈。DroidDeck 的大屏使用 ANGLE/Vulkan 加速 CEF 和 gamescope；它仅在 KDE 桌面路径禁用 CEF GPU。我们当前客户端是 CEF 软件绘制→Xephyr→XPutImage→Android GL，不能仅照搬它的 CEF 参数就获得硬件加速。Windows 游戏不走这条显示链。

以上 v23 时游戏下载和存档仍是两套目录。用户随后明确暂停键盘工作，优先共用目录；10 月 17 日的新实现及证据转入[共享库记录](steam-shared-library-20261017.md)：新库使用 SteamPSP，旧 XRGameNative 原地复用，Official/Custom 指向同一 steamapps 与既有 Wine prefix。AYN 官方识别和公共运行库实际下载已通过；游戏内读档和官方 Play 生命周期须单独验证，不能从目录链接推定通过。

认证暂保留两次登录。没有发现受支持的官方客户端登录态导入接口；JavaSteam token 与官方桌面会话不能直接视作同一种凭据。游戏启动仍检查双方账号相同，未复制/提取认证文件，不把 Bionic pipe 检查成功当成游戏联机通过。
