# Half-Life: Alyx 加载插页：修复 OpenComposite 下的长时间灰屏（2026-10-09）

- 状态：Swan 上已验证。startup 和 a1_intro_world 两次载入都显示了加载面板和“按下 Trigger 以开始”，按扳机后正常进入游戏。
- 证据目录（仓库外）：`xrgame-native-evidence/swan-20261008/swanxr-ahb/run24-swan-final/`（问题）和
  `run25-interstitial/`（修复）。录屏、logcat、OpenComposite 日志和 APK 都在其中，不提交。

## 1. 现象与根因

- 录屏 `ScreenRecording_2026.10.09-13.31.54`（原文件没有收尾，修复为仅视频的版本后才能看）：主菜单之后有约 36 秒的均匀深灰屏
  （平均亮度约 46），直到按下扳机。用户怀疑是视频播放失败；实际上 Valve 开场 logo 和主菜单都正常。
- OpenComposite 日志：Alyx 通过 SteamVR 的 `IVRMailbox`，向 `hlvr/interstitials` 依次发送 `set_paths`（webui、models 路径）、
  `begin_loading`（地图、“正在加载”、说明、小窍门、朝向角）、`show_message`（“按下 Trigger 以开始”）和 `end_loading`。
  加载界面原本由 SteamVR 绘制。上游 OpenOVR（`a27e7e6a`）的 `BaseMailbox::SendMessage` 只打印一行
  “Pretending to send mailbox message”，`FadeGrid` 不做任何绘制，`SetSkyboxOverride` 也只提交一帧 quad，而我们的 runtime
  不处理 quad 层。所以头显里一直停在游戏淡出后的最后一帧，也看不到需要按扳机的提示。

## 2. 修复

| 层 | 改动 |
|---|---|
| OpenComposite | `references/opencomposite/patches/mailbox-forward.patch`：经 `xrGetInstanceProcAddr` 取得 `xrSendMailboxMessageGNX`，存在时把 mailbox 名称和消息转给它，否则保持原行为；`build.ps1` 检查产物中包含该特征字符串。OpenVR 2.15.6 头文件的校验改为先统一换行再计算哈希（Windows 检出为 CRLF） |
| PE runtime | `xrSendMailboxMessageGNX`：校验 mailbox 名称，消息上限 6000 字节，base64 编码后发送控制命令 `MAILBOX target= data=`；unix 快速路径的请求缓冲扩大到 8 KB（ABI 8），较大的缓冲改为由 `gn_lock` 保护的静态存储（该 DLL 不带 CRT，链接不了栈探测函数） |
| App | `WindowsVrControlServer` 接收 `MAILBOX`，单行上限 8192；`WindowsVrInterstitials` 解析三类消息并去重；`WindowsVrInterstitialRenderer` 绘制 1600×900 的面板 |
| XR 线程 | 插页显示期间，在视线正前方 1.6 m 处固定一个 1.4 m 宽的 quad，不提交投影层。游戏帧照常领取和释放，以免游戏因等待 swapchain 卡住、无法响应扳机 |

OpenComposite DLL 为 `2c3f368ba554ab1282dea0000ba2af4de9bbd9c3b0f6f530bf584e02d846ad66`，用 MSVC 19.51（CMake 默认选中的 VS 18
Insiders）和 Vulkan SDK 1.4.304.1 编译。与上一版 `12eee850…` 相比，导入表只多了 Wine 已实现的 `KERNEL32!CreateFile2`。
`tools/xrgame/opencomposite-pin.json` 记录了补丁、构建脚本的哈希和编译器信息。

## 3. 设备验证（Swan，APK `eab515f4406326b9ac041049058781c1a35e936f34b3d3f07884a5b91b854037`）

| 地图 | begin_loading | show_message | end_loading |
|---|---|---|---|
| startup | 14:17:45.37 | 14:17:58.09 | 14:17:59.19 |
| a1_intro_world | 14:21:32.18 | 14:22:02.47 | 14:22:04.14 |

- OpenComposite 日志对每条消息都记录 “Forwarded mailbox message”。logcat 记录两次 “Windows VR interstitial shown 1600x900”；插页相关的
  OpenXR 调用没有失败。用户在头显中确认看到了面板和提示。
- 单元测试：`WindowsVrInterstitialsTest` 4/4，`XrGameRuntimeTest` 5/5；APK audit 131 个文件，0 错误。

## 4. 未覆盖

- 面板只显示文字，没有 SteamVR 原版的 webui/3D 模型，也没有使用 `facingAngle`（改为当时的视线方向）。2026-10-09 评估过完整还原：
  - 原版是 Valve vrwebui 的 React 页面加 SG 场景图，mailbox 通过 `ws://localhost:27062` 连接。
  - 3D 部分为 `models/ui/dioramas` 的 OBJ 和 8K BC4 DDS，以及 `map_hologram_steamvr` 的 OBJ 和 BC7 AO/线框贴图。
  - 需要的条件：
    - Swan 的 GLES 原生支持 BPTC、RGTC、S3TC；
    - 系统 WebView 为 Chromium 138；
    - Foundation lite-engine 只支持 Vulkan、不读 OBJ、不支持 BC4，许可也只覆盖 DebugBus。
  - 用户决定保持现在的文字面板。
- 只验证了 Alyx。其他使用 mailbox 的 SteamVR 游戏会被转发，但目前只处理 `hlvr/interstitials`。
- OpenComposite 补丁和构建脚本的改动还在 `references/opencomposite` 的工作区里，没有提交到 `tencentmalos/opencomposite`。
