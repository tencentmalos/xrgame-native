# MHR：小米 Android 16 启动与 Mapper 5 修复

2026-10-05，`feature/malos/api-replay`，基线 `470d1deca953bd0b5334890dcc555f6771f1204b` 加本次 Wine 执行及 Mapper 修复。
使用 Xiaomi 15 Pro，`picoXrDebug` APK、native **RelWithDebInfo**（`-O2 -g -DNDEBUG`）。
修复包已保留数据安装，观察到 MHR《曙光》标题和原存档据点，画面正常。
这次是启动及显示兼容性验证，不是性能、联机、长期稳定性或 Swan 验收。

## 两个阻塞点

### Wine 内部重新执行 loader 被拒绝

原包 Wine PID 24441 报 `wine: could not exec the wine loader` 后退出，Android 应用仍在。
应用最初通过 `/system/bin/linker64` 启动 Wine，但 Wine 内部的 `execv` 和
`posix_spawn` 仍直接执行 app 私有目录中的 loader / wineserver。

应用域探针 PID 24642 证实：APK native 目录直接执行成功（约定退出码 37），
私有目录直接执行返回 `EACCES`（13），通过系统 linker 执行成功；文件 RX/RWX 映射及
匿名 JIT 映射探针也成功。因此这里不能归因为 FEX 的 JIT 映射失败。

新增源码构建的 `libxrgame_wine_exec.so`，只在原始执行返回 `EACCES` 后适配：

- 目标必须位于应用 `files` 根目录内，是应用所有、具有执行权限、无 setuid/setgid 的普通文件。
- 检查 ELF64 / AArch64 / ET_DYN、程序头边界和 `/system/bin/linker64` 解释器。
- 仅适配 Wine 的 `argv[0] == path` 调用约定，保留后续参数、环境和退出状态；自定义 argv0 不改写。
- 静态 `wine-preloader` 保留原失败结果，由 Wine 既有路径回退到动态 loader。
- 不修改 SELinux、W^X、游戏文件或系统执行策略；未使用 proprietary redirect 实现。

两条 picoXr Wine 启动路径均注入适配库和 `XRGAME_EXEC_ROOT`。上游 flavor 行为不变。

### Turnip 无法加载设备 Mapper 5

解决 loader 后，游戏可以初始化 D3D12，但出现彩色条纹。Wine 日志显示
`/vendor/lib64/hw/mapper.qti.so` 对 default linker namespace 不可访问。
独立探针确认：普通 `dlopen` 失败，经设备系统 SP-HAL helper 加载成功。
Turnip 无法使用 Mapper 5 时退到旧元数据路径，本机 AHB 布局解释不正确。

新增仅在 Wine 预加载的 `libxrgame_wine_mapper.so`：对上述精确 Mapper 路径调用系统
`libvndksupport.so` 的 `android_load_sphal_library`，其余加载保留原行为。
没有随包分发 Qualcomm HAL，也没有改变 AHB GPU 拷贝显示路径。

转发普通 `dlopen` 必须保留原调用者返回地址，因为 Bionic 据此选择 namespace。
实现使用 Clang `musttail` 强制尾调用；依据见
[AOSP libdl.cpp](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libdl/libdl.cpp)。
临时探针的普通包装调用曾破坏 namespace，最终实现已经移除该问题。

## 验证

| 检查 | 结果与边界 |
| --- | --- |
| 构建与打包 | 全新 APK 打包成功；源码审计 126 项、0 错误；设备 APK SHA 与本地一致 |
| 应用域执行探针 | PID 13279，UID 10425，`untrusted_app`；15 个执行/映射用例均符合预期，0 signal、0 timeout；Activity 记录总时长 201.740261 ms |
| Wine 参数保留 | `execve`、`execv`、`posix_spawn` 均退出 37；argv0、空参数、空格、中文和环境值一致 |
| 负面用例 | 缺失文件、非 ELF、无执行权限仍失败；Bionic spawn 的子进程 127 计为执行失败，未误报运行成功 |
| 映射基线 | 文件 RX/RWX 映射成功；匿名 RW→RX、RWX 代码均返回 42。自定义 argv0 的私有目录直接执行仍按预期被拒绝 |
| 生产 Mapper 库探针 | Vulkan instance/device、AHB 分配/属性读取、image 创建、内存导入和绑定均成功；1280×720 RGBA，3,686,400 字节。此独立探针在 run-as 域运行，与应用域证据分开 |
| 实际游戏 | MHR PID 13967，正常显示启动图、曙光标题及原角色据点；最后状态采样时同一 PID 已存活 444.42 秒，非全部在场景内的时长 |

启动时出现联机错误 `MR-111-83`，关闭后可继续。该错误没有导致进程退出；本次未验证联机。
场景截图包含用户操作后的状态，不将其描述为完整自动化存档回放。未执行战斗或长期游玩测试，
也未在 AYN 上回归新适配库。屏幕瞬时 FPS 不作为性能结论。

### 失败尝试与清理

- 仅加入 exec 修复的中间包 SHA：`8aab12e8256ef0d41b3323255ced1814c1657abe958b1172ce5e207f43d014c4`。
  可进入 D3D 初始化，但尚未修复 Mapper 花屏。
- 中间诊断尝试出现既有 `NtRaiseHardError 0x50000018 → 0xdeadc0de`；本次不能据一次成功启动
  宣称所有历史 MHR 间歇性退出均已解决。
- 临时全局 dlopen 包装曾导致构造期空指针、AHB 分配失败；后续临时 AHB 日志包装因 Wine
  局部加载组中 `RTLD_NEXT` 无法解析目标再次产生空指针（PID 11920）。这些均为诊断探针引入，
  未进入最终源码。原始日志/maps/失败截图保留，不能把它们当作原始游戏 OOM 或正式修复包崩溃。
- 最终复测前已恢复原 `envVars`，校验一致；临时 `LD_PRELOAD`、WINEDEBUG/VKD3D_DEBUG 配置已移除。
  DebugBus 服务已停止，未建立 LLDB 会话。清理临时二进制文件时 USB 已断开，因此设备私有探针
  文件尚未删除；它们已不被启动配置引用。未清 shader cache、替换游戏文件或修改存档。

## 身份与证据

- APK SHA-256：`8d4f5c69f52dfedcc6ecc0e0dd5ec74a75c3cf6ec242507a96c2230b0ed13ed4`。
- 组件 catalog SHA-256：`e8f2780a71024d47b4f12ff1b74616354f588573d77ae105d8c93162337732bb`。
- 设备 Xiaomi 15 Pro / `2410DPN6CC`，Android 16 / API 36，targetSdk 36，4 KiB，SELinux enforcing。
- Build：`Xiaomi/haotian/haotian:16/BP2A.250605.031.A3/OS3.0.308.0.WOBCNXM:user/release-keys`。
- Boot ID：`7bbc7d32-95ce-4099-bc9f-692fdcf67f4a`。
- MHR PID 13967，启动 boot 秒 `731801.96`；最后采样 `732246.38`，存活 `444.42` 秒。
- ExecProbe PID 13279，父应用 PID 5589；应用 elapsed 时间 `731768435151234` → `731768636891495` ns。

| ELF | Build ID |
| --- | --- |
| `libxrgame_wine_exec.so` | `14584bd6a2854bc68752ea5a7f08f5cd950ea790` |
| `libxrgame_wine_mapper.so` | `8b5aa695aa7ded79c6d5fbe569c9de83bd78ccd9` |
| `libxrgame_exec_probe.so` | `54590e2c22236af9e4959a28409ea37121127eae` |
| `libvulkan_renderer.so` | `dd64d7f47d1da72b319ad6c089f19750e56936eb` |
| `libxrgame_debugbus.so` | `e7074fc7d7602871596e4668959b678047f898e6` |

私有证据位于仓库外 `../xrgame-native-evidence/mhr-xiaomi-20261005/`：
`validation-final.json` 保存完整 APK ELF Build IDs；`exec-final.jsonl`、`exec-final-activity.json`、
`ahb-final.txt`、`audit-fix2.json`、`build-fix2.log` 和 `final-scene-stat.txt` 保存测试依据。
`final-after-close.png` 是标题，`final-menu.png` 实际是已载入的据点场景；文件名不作为状态判断依据。
APK、游戏画面、账号及原始日志不提交公共仓库。

AYN 的历史启动/呈现问题见 [黑屏分析](mhr-black-screen-20260928.md) 与
[异步呈现验证](mhr-present-async-20260929.md)，不能用这次小米结果替代原有设备验收。
