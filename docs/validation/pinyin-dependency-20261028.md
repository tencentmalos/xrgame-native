# XR 拼音依赖补全（2026-10-28）

这是源码交付与构建检查记录，不是新的 APK 或设备验收。

## 原因与恢复

`dffd6a4e9` 提交了 XR 键盘的拼音适配和词库打包任务，但没有登记 PinyinIME
子模块。原实现从仓库外复制引擎；CMake 缺少引擎时只给 warning，Gradle 缺少
词库时生成空资源目录，APK 审计也没有把词库列为必需项。因此新机器能构建出
没有中文输入的版本。

原始修复找回于 Windows 的 `C:\workspace\third_party_scratch\PinyinIME`；
这台主机的 WSL **Ubuntu**（Ubuntu 24.04）中仍有 `/root/xrgame-native`
构建副本。新构建不再需要这些机器路径或手工副本。

依赖按用户要求放在 **Foundation 内作为子仓**：

- [tencentmalos/PinyinIME](https://github.com/tencentmalos/PinyinIME)，维护主干
  `malos/main`，pin `97886655ccef86d02dc2a46841026a86dbfee553`。
  通过 AOSP GitHub mirror 建 fork，保留 AOSP `49aebad1c1cfbbcaa9288ffed5161e79e57c3679`
  的历史及两个原提交：`b72d4ad` 修复 64 位词库计数读取，`9788665` 修复 NDK 外部构建头文件。
- Foundation `third_party/pinyinime` gitlink 与独立 `modules/pinyin` 构建入口：
  `addcb9bf`，从 SteamPSP 原 pin `b42ab4c7` 建功能分支，已通过 `ffbe6440`
  合入 Foundation `main`。主仓固定 `addcb9bf`，以保留已编译验证的最小依赖变化。
  合并时 `modules/CMakeLists.txt` 的新增模块位置冲突，保留了主干的 `online_content`
  开关和新增的 `pinyin` 开关，两者均为显式启用。
- 主仓集成 `dffd6a4e9` 及其前置提交，使 XR 宿主、启动面板、IME streaming、
  Wine Unicode 输入和拼音适配一起进入 `malos/main`。后续的滚动/Turnip 诊断
  提交 `5b5692fee`、`0a69e3ecb` 不属于本次拼音依赖集成。

PinyinIME 引擎、词库和上游 `NOTICE` 为 Apache-2.0。Foundation 的授权范围不变；
没有把其源码复制进公共主仓，也没有上传 APK。

## 新机器初始化

在主仓目录执行（有 Foundation 私有仓访问权限的正常开发环境）：

```sh
git submodule update --init foundation
git -C foundation submodule update --init third_party/pinyinime
```

不需要复制 `app/src/main/cpp/extras/pinyinime`，不使用 `--remote`，不递归初始化
Foundation 的研究参考仓。其余 APK 依赖继续按[本地构建文档](../building/apple-container.md)准备。

SteamPSP 链接 `spatial::foundation_pinyin`；Gradle 从
`foundation/third_party/pinyinime/res/raw/dict_pinyin.dat` 打包资源。词库 SHA-256
仍是 `6bf0bbde4e3134cce38d08524a9f4dc1af40435243c8e36b38eb68d1e14462b2`。
引擎/词库缺失时 CMake 失败，词库缺失或 SHA 不符时 Gradle 失败；APK 审计要求
`assets/pinyin/dict_pinyin.dat` 存在并匹配 SHA。原生构建 provenance 记录 PinyinIME
revision/dirty 和词库 SHA；Gradle 原生输入包含 Foundation 的模块与引擎源码。
Apple Container 的同步脚本会复制嵌套依赖，源端缺少它时立即报告初始化命令。

## 本次验证

隔离目录：WSL `/root/pinyin-dependency-check`。原有 WSL 构建副本未修改。

- Foundation 独立模块，Linux x86_64 / GCC 13.3：CTest **1/1 通过**。
  测试逐字输入 `nihao`、`zhongguo`，检查候选“你好”“中国”、选词固定两字、
  flush/关闭/重新打开用户词库，共 8 轮。
- NDK **27.3.13750724** / Android API 29 / arm64-v8a：独立引擎及测试程序编译通过。
- 同一 NDK 冷构建 SteamPSP 完整 `xrimmersive` 目标通过（RelWithDebInfo，
  `XRGAME_BUILD_PROBES=OFF`）；符号中包含 `PinyinComposer::available` 和
  `im_open_decoder`。`libxrimmersive.so` SHA-256：
  `2560327830bad27bd1ceadd837035ba57499f29367cb5592273e978adf9ec231`；
  Build ID：`aefe73e3569d551ad1fa7bd062543cddfe6f9d2b`。
- 隔离主仓源码快照使用缓存的 Gradle **8.12.1**，实际运行
  `:app:stagePinyinPicoXrDebug`：正常词库打包成功，输出逐字节等同源词库；
  缺词库触发 `input_file_does_not_exist`；损坏词库触发 checksum mismatch。
  后两项是预期失败，日志保留。
- 独立 CMake fixture 缺少嵌套子仓时明确失败。APK 审计使用仅含拼音规则的
  ZIP fixture：正常词库通过，缺失/损坏均失败。这不是完整 APK 的审计结果。
- 修改的 shell 脚本通过 `bash -n`；`git diff --check` 通过。
- 从 GitHub 干净克隆主仓的集成提交，再仅初始化 `foundation` 与
  `third_party/pinyinime`：分别取回 `addcb9bf` 和 `97886655`，词库 SHA 一致。
  没有借用旧机器的源码目录，也没有初始化 Foundation 的研究参考仓。

构建检查的中间失败：首次 native 配置缺少新 worktree 的 `lsfg-vk-android/thirdparty/dxbc`，
按已有 gitlink 初始化后通过；本地检查脚本最初假定了 Gradle 资源输出目录和英文错误文本，
改用 AGP 实际输出目录与错误标识后通过。中间失败可见本次工具输出；缺失/损坏词库的
预期失败日志保留在上述隔离目录。

未在本次操作中生成或安装完整 APK，未运行 Mac 的 Apple Container 或重新操作头显。
过去的 Swan 中文输入证据仍见[原记录](swan-official-steam-20261019.md)。
