# Litep 主流程覆盖与开销控制（AYN，2026-10-02）

已把 host 插桩从启动准备扩展到下载概览、环境生命周期、异步 Present、AHB copy、Vulkan
等待/呈现与 Windows VR SBS bridge。默认不采集，显式采集默认 coarse，detail 按需展开。
关闭采集时 Java region 不再创建 AtomicBoolean/闭包；原生队列不增加 callback 分配。
覆盖矩阵和使用方法见 [Litep](../debugging/litep.md)，数值及原始文件哈希见
[机器可读记录](litep-coverage-20261002.json)。这不是完整 guest/GPU 覆盖或 FPS 性能验收。

## 身份与配置

| 项目 | 本次身份 |
| --- | --- |
| 主仓 | `6193851f6ceb1f126839bc0ed2e2e7c5a68c9f1f` + 工作区修改，含此前 SBS 影院改动；未 commit/push |
| APK SHA-256 | `9d8ef4e89a14d7b97e263a2859828b8e8ca71525489817086a19adab925e4222` |
| Catalog SHA-256 | `dbf85e43cfb5e1f604507a5cb466cd4ffb71e57560a4830b33f02a6fadb8935f` |
| libxrgame_debugbus Build ID | `8870cb9946e98ee7bace9c44359943c57660e37c` |
| libvulkan_renderer Build ID | `2151525d5ac80b73f293814fba05741c15c618b9` |
| libxrimmersive Build ID | `67ce8e84096bc52e98dc583441bd10c8780981b2` |
| Foundation / SDK | `c1d339c3a9b231ad23d02e4fd982e3e7120f7ef5` / `0b467a861569345a64fd88cb2e5daa8ff542c14f`，均未修改 |
| 设备 | AYN Thor，API 33，4 KiB，targetSdk 36 |
| Build | `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys` |
| Boot ID | `8bb14501-5800-4b9b-a9ab-7173603812f7` |
| Host / guest | host PID 11333；MHW PID 12660；wineserver PID 12547 |
| 场景 | MHW 启动到离线提示，普通游戏 SBS 影院，AHB async copy，host FPS limiter 关闭；未进入可玩场景 |

APK 从清空旧输出后的打包生成，`tools/audit-apk` 检查 124 个文件、0 错误；安装保留数据。
实机按准确名字查询 renderer/debugbus Build ID，与 APK 一致。总 modules 列表达到上限被截断，
不把该列表视为完整 guest/host 映射。SDK owner 的导出符号仅由 debugbus 定义，两个 renderer
库通过动态依赖引用；没有每个 `.so` 各静态链接一份 profiler。全部 APK `.so` Build ID 在 JSON。

原始 PROF、sidecar、截图、构建日志、源文件哈希清单保存在仓外
`../xrgame-native-evidence/litep/20261002/`，不提交账号、设备序列号、APK 或 Foundation 源码。
APK 生成后仅对 `configureGraphics` 新增 use 块调整缩进，并补充测试和文档，无行为修改。

## 实机覆盖与数据量

| 捕获 | 数据事件覆盖时长 | 完整 Present 区间 / markers | CPU scopes | 压缩文件 |
| --- | ---: | ---: | ---: | ---: |
| coarse 启动 | 101.101 s | 4675 / 4676 | 28083 | 624530 bytes |
| detail 固定离线提示 | 20.004 s | 1197 / 1198 | 11978 | 254073 bytes |
| coarse 同一提示 | 20.011 s | 1197 / 1198 | 7186 | 158914 bytes |
| Home / 返回 / 快捷菜单 | 2.533 s 有事件；file 上限 40 s | 3 / 4 | 58 | 1759 bytes |

两次固定场景采集，coarse 少约 40% CPU scopes、37.5% 压缩数据；Present 区间平均值
分别为 16.715 ms（coarse）和 16.709 ms（detail）。**不能据此宣称没有 FPS 损失**：这是单组
顺序采样、受约 60 Hz 节奏限制，未固定频率/温度，也没有无插桩 APK 对照。scope 文件大小
不等于运行时总内存；ring 容量按线程计算，不能外推固定秒数。

启动捕获中，launch.preparation 为 2555.846 ms，runtime.prepare 为 1665.321 ms，
runtime.driver.stage 为 23.842 ms。X server 建立到首个 Present **请求**为 9224.664 ms；
这不是首个可见游戏画面。主要启动 region 在不同 coroutine/线程间正确配对。

固定画面的 coarse 能看到 copy、取得锁、fence、acquire 和 QueuePresent：例如 acquire 的
p50 为 14.144 ms，copy p50 为 1.735 ms、fence p50 为 0.725 ms；copy lock_wait p99 为
0.001 ms。这只说明该 host 场景的调用耗时可分辨，不据此推断游戏场景瓶颈或 GPU 利用率。
detail 额外记录 command recording、QueueSubmit 和 completion callback，coarse 不含这些事件。

生命周期文件记录两次 environment.pause，以及 Surface detach/attach 和 device_idle。
未记录到 environment.resume/stop 或 wine.process.exit；不把这次捕获算成这些桩点的设备验收。
随后通过快捷菜单退出，确认 MHW/wineserver 已结束、host 仍存在、activeSteamAppId 为空。
运行时记录已停止、模式恢复 coarse、诊断服务停止、临时 probe 目录清理。

## 完整性与负例

四份文件均有完整容器尾部，decoder chunks_skipped=0、dropped_chunk_markers=0，未发生
字节截断。但启动、coarse、detail 各有 **2 个未闭合 CPU scope、4 个未配对 region**：采集结束
时 host 仍在 acquire/呈现循环、队列有在途任务，符合边界截断。保留告警与原文件，不补造
结束时间，也不把已解码配对称为整个运行过程无损。生命周期文件无解码告警。

合成 fixture 额外验证了以下情况：

- 正常：PID 10676，6 个已闭合 CPU scopes，cookie 从 10676 到 worker 10677 精确配对，
  无解码告警；detail 在 coarse 下被过滤，显式 end 加析构不会重复关闭，return/throw 路径关闭。
- 过期 generation：PID 10681，旧 scope/region 的结束被拒绝；留下 1 个 open scope 和 1 个
  unpaired region，符合故意中断的负例。当前解码器可能把新 epoch 的 scope 嵌到旧开放 scope
  下，分析时必须分开 generation，不能依此算跨边界的 self time。
- 初次负例：PID 8517 的 worker 提前退出，ring dump 丢失该线程端点。当前 SDK TLS 在退出时
  取消注册 ring buffer；Streaming 则先 flush。该失败捕获保留，不能通过让测试线程存活来
  宣称已修复 SDK。长启动/退出流程采用有界 file capture，Foundation 本次未修改。

scrcpy session 提前以 native_exited / code 0 结束，未获得截图；保留诊断日志路径，改用 ADB
截图验证 SBS 离线提示。uiautomator 曾返回 null root；保留失败输出，未据此推断游戏内容。
构建初期的任务名、CMake link 签名和 C ABI POD 编译失败均修复，失败日志仍保留。

## 开销微基准与检查

独立 NDK probe PID 15873，SHA-256
`a9c97bae4dadf33a553f9928872385b599d66743fd38b82e3166c1dc77c74590`，Build ID
`e7bea5f58eab1695e1051d8befba985fed8b6cea`。每种情况预热 1000 次，再运行 7 轮 × 100000 次。
下面是每轮每个空 scope 对/循环体的时间，不是一次完整游戏帧：

| 情况 | 中位 ns | 最小–最大 ns |
| --- | ---: | ---: |
| 无 scope 的循环基线 | 2.482 | 2.481–2.482 |
| ring 关闭，coarse gate | 8.930 | 8.533–10.413 |
| coarse 记录 | 89.761 | 74.096–161.451 |
| detail scope 被 coarse 过滤 | 1.883 | 1.883–1.883 |
| detail 记录 | 73.644 | 73.455–73.888 |
| instrumentation off | 1.883 | 1.883–2.013 |

频率和 CPU affinity 未锁定，不从不同情况的差值估计精确固定开销；detail 记录比 coarse
记录数字小不代表它更便宜。测试不含 JNI、driver、GPU 或磁盘 Streaming 开销。初次微基准
也保留；最终使用有 PID、可对应 probe 哈希的这次结果。

检查结果：7 项 JVM 单测通过；Present queue 既有阻塞/失效/失败/关闭矩阵分别在启用和
编译掉桩点的配置通过（5 次接收对应 5 次开始/结束，拒绝任务没有 region）；无桩版本
未引用 Litep 符号；NDK probe、原生库、Kotlin 和完整 APK 构建通过。

下载事件仍待真实下载捕获验证；Windows VR bridge、同步/直接采样路径、完整退出与恢复、
guest 内部和 GPU 计时不在本次实机验收范围。Swan 验证仍延后。
