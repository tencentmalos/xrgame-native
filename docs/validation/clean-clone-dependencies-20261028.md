# 主干干净克隆依赖审计（2026-10-28）

范围：在 Windows / WSL 上核对 `malos/main` `55028cc9c` 的远端源码依赖与旧机器的暂存输入。
用户随后要求能源码构建的组件不上传二进制仓库，并由另一台机器执行实编验证。本轮未运行完整 APK 构建、Mac Apple Container 或设备测试。

## 已确认与修复

- 从 GitHub 新克隆主仓并按 gitlink 初始化 10 个 APK 必需的直接/嵌套子仓，全部成功：Foundation、JavaSteam、OpenComposite 配方、Vulkan-Headers、fmt、adrenotools、lsfg-vk-android，以及 PinyinIME、linkernsbypass、dxbc。未复制旧机器的源码树。
- PinyinIME `97886655` 的词库 SHA 为 `6bf0bbde4e3134cce38d08524a9f4dc1af40435243c8e36b38eb68d1e14462b2`；Foundation pin `addcb9bf` 已可从远端恢复。
- OpenComposite 打包 pin 原为 `ce6bafc1…`，运行时和 APK audit 则要求 `25509aaf…`。统一为已有设备验证记录、旧构建机实际保存的 `25509aafee09c2ab8f8d523e8b30dba7e32ee395d9809db4d3be0c1e70f3c188`；保留前一候选构建信息。Gradle 从 pin 生成运行时校验常量，预检检查 APK audit pin 与之相同。
- `prepare-linux.sh` 原只浅取 Mesa `d15b7c01`，但 Android Turnip、Linux Steam GPU 及打包配方均要求 `55186f1f`。准备脚本同步到后者，消除依赖旧机器 Git 对象缓存的问题。
- Apple Container 同步排除 Foundation 实际的 `reference/` 研究目录；之前仅排除了 `references/`。源码 pin 记录增加嵌套 PinyinIME。
- 新增 `check-apk-inputs.py`：按固定 gitlink 初始化 APK 子仓，检查词库、OpenComposite 配方/DLL、游戏运行时默认版本/哈希、PulseAudio 和 Linux Steam 必需文件/哈希/源码一致性。缺少或过期的依赖在启动编译前报告。

## 仍需源码准备的本地输入

旧 WSL 的暂存件约 400 MB：游戏 bundle、PulseAudio、OpenComposite、Linux Steam userspace。
当时 `tencentmalos/xrgame-native` 的 GitHub Release 列表为空；catalog 中的 Release URL 不能作为现成下载入口。
没有上传这些暂存件，没有创建私有二进制仓库，也没有上传 APK、密钥或私有 capture SDK。

旧暂存件中，游戏 bundle 的 catalog SHA 为 `fed4d3ff5440e48f56c140b8d8ab37aaf44d2712f645bf6982a54dae23826d6b`，
7 个组件的实际哈希均与 catalog 相同。PulseAudio 与 OpenComposite 也通过各自记录的哈希检查。
这证明旧文件的自洽性，不证明它们能够从当前配方一次性重建。

Linux Steam 暂存包虽然内部 manifest 哈希自洽，但 `launch-client.py` 含未合入本次主干的滚动实验，
兼容库的 `adapterSha256` 为 `9a8f1a5b…`，而主干 `client-proc.c` 为 `07b907fd…`。
新预检会拒绝这种暂存包。须用当前 `linux-steam/stage.py` 重建适配件和 manifest；未把旧暂存件重新盖章伪装成当前源码。

## 另一台机器的验证入口与边界

先执行：

```sh
python3 tools/xrgame/check-apk-inputs.py --init-submodules --sources-only
./tools/xrgame/apple-container.sh setup
# 按构建指南中的源码配方生成必需的运行时暂存目录后：
python3 tools/xrgame/check-apk-inputs.py --report build/apk-input-audit.json
./tools/xrgame/apple-container.sh build
```

详见[构建指南](../building/apple-container.md)的组件入口。尚未验证的干净构建环节包括：

- OpenComposite 的 OpenOVR 源码包、Vulkan import definition、xwin SDK/CRT/ATL 准备；它们不是当前 `setup` 的自动产物。
- 从历史 Proton 基础产物生成当前 `linux-steam2`、当前纹理质量组件与完整 catalog 的链路；旧全量 preview 配方输出不能直接替代当前版本要求。
- Linux Steam ARM64 userspace、Wine 适配器和 GPU 组件的从零构建、暂存；私有 debug 签名密钥需由用户在本机提供。

因此本轮可以确认 APK 源码子仓已远端闭环，并修复已发现的 pin 漂移；尚不能声称“只 clone + setup 就能完整编译”或“Mac 整包编译通过”。
