# MHR：异步呈现对照与配置入口

2026-09-29，AYN Thor。延续 [MHW 的 fence 等待改造](mhw-present-async-20260928.md)，
验证 MHR 是否同样受 X 请求线程阻塞影响。仅覆盖本次据点场景，不是 Swan/XR 或长时间游玩验收。

## 同包 A/B/A

MHR 原角色、曙光据点固定视角，同一游戏进程依次同步 A1 → 异步 B → 同步 A2，
每段 30 秒。保留游戏/运行库/缓存，不调整画质、亲和性或电源模式。
游戏设置实机复核为 1280×720、中等画质、全屏、60 Hz、限帧 60、**垂直同步关闭**；
应用快捷菜单 FPS 限制器在三轮开始前已关闭。因此收益不是关闭游戏垂直同步或提高帧率上限。

| 指标 | 同步 A1 | 异步 B | 同步 A2 |
| --- | ---: | ---: | ---: |
| SurfaceFlinger 实际呈现 /s | 28.47 | 42.25 | 29.04 |
| X11 Present 请求 /s | 28.87 | 42.21 | 29.08 |
| 呈现间隔 p95 /ms | 66.89 | 33.44 | 66.86 |
| KGSL 区间 busy 均值 | 74.25% | 96.94% | 73.82% |
| GPU current 均值 /MHz | 585.2 | 680.0 | 591.7 |
| 系统总 CPU busy | 47.45% | 63.17% | 48.75% |
| 热 guest TID 31064 on-CPU | 48.96% | 68.16% | 50.22% |
| 约 8 秒 perf 内该线程 surface capabilities 查询 off-CPU /s | 2.557 | 0.323 | 2.654 |

相对两侧同步均值，呈现速率提高 **46.9%**，查询 off-CPU 下降 **87.6%**。
GPU max_freq 三轮均为 680 MHz，但没有锁频，异步下 governor 更持续达到上限；
这是正常调频下的整条路径收益，不是固定频率微基准。off-CPU 也包含调度等待。

同步热线程停在 `xcb_wait_for_reply → x11_surface_get_capabilities2 → winevulkan`；
X 请求线程则等待 `vkWaitForFences → nativeCopyWindowContentAHB`。
异步 B 的请求接收/入队返回位于 TID 30618，copy dequeue/done/Complete/Idle 移到 TID 30613。
receive → enqueueReturn 均值 0.300 ms，p95 0.868 ms；copy fence 等待均值仍为 18.269 ms，
但不再占用 X 请求线程。没有 GPU timestamps，不能把 fence elapsed 当成纯拷贝成本。

完整 trace：897 / 1331 / 914 帧，各轮 0 顺序错误；每轮 2 个边界不完整帧保留并排除。
simpleperf 使用 99 Hz cpu-clock、off-CPU、FP 栈，每轮约 8.05 秒，均 0 lost samples。
SurfaceFlinger 排除首份历史数据、无效时间戳并去重。原始查询栈全量聚合，未只统计头部栈。
数据见 [JSON 摘要](mhr-present-async-20260929.json)。

## UI 与行为

- 入口：游戏详情 → 齿轮 → **编辑容器 → 图形 → 异步呈现（实验性）**。
- 每个容器独立保存，默认关闭；已有 `XRGAME_PRESENT_ASYNC_COPY=1` 配置自动显示为开启。
  UI 沿用该变量，不创建第二份配置。保存后重启游戏生效。
- 需要 Vulkan、DRI3，并关闭快捷菜单中的 FPS 限制器；游戏自身限帧可以保留。
  直接 AHB 采样、wait/idle fence、偏移等不支持的请求仍不纳入优化。
- 快捷菜单 FPS 限制器启用时，在已接收 copy 归还后切回同步；关闭限帧后恢复用户的异步选择。
  切换期间不改变在途任务的归还语义。`requestedFrameRateLimit` 与当前 `frameRateLimit` 分开报告。
- picoXr 的启动配置读取不再受 DEBUG 限定；DebugBus 对照开关仍仅限内部 debug 包。
  本次实际构建、安装、设备验证只使用 picoXrDebug。

UI 保存实测仅新增异步环境变量，其他环境变量保持不变。原有容器编辑器同时规范化
DXVK 配置：`async=1 → 0`、补入 `vkd3dFeatureLevel=12_1`，并标记 `config_changed`；
这些不是同包 A/B/A 的变量，不能把后续新包运行拼进前三轮对照。

## 暂停回归

旧包在 Shift+Esc 打开快捷菜单后发生一次 ANR：guest 已 SIGSTOP，遗留 Shift 的
autorepeat 在主线程持续向 X socket 写入，最终阻塞 `ClientSocket.write`。
这次卡顿与 GPU fence 无关。保留 ANR/dropbox/logcat/进程状态后，只恢复该会话已暂停的
guest 进程，再通过“等待”恢复应用；原游戏 PID 未变，随后正常保存退出。

修复在 picoXr 暂停 guest **之前**停止键盘重复并发送所有已按键的 release，避免暂停吞掉
keyup。两项 Robolectric 回归通过：按键/修饰键释放及迟到 keyup 幂等；限帧切换等待在途
copy 归还并保留用户 opt-in。此修复不是全面的 X socket 非阻塞改造。

新包实机完成两次 Shift+Esc → 暂停至少 12 秒 → 点击恢复，应用仍响应且回到同一场景、
同一游戏 PID，无新 ANR。快捷菜单启用 FPS 60 后报告 requested=true、active=false；
关闭限帧并恢复后 active=true，完成计数继续增加。设备切换是在暂停菜单中操作；
在途 fence 的退队切换由单元测试覆盖，未将两者混称为设备故障注入测试。

## UI 包独立复核

从 UI 开启、保存、重新打开确认勾选，再启动游戏；未发送 `present async_copy` 覆盖命令，
启动即报告 requested/active=true。进入原存档据点后再采集 30 秒：

- Present **42.04/s**，SurfaceFlinger **42.11/s**，呈现间隔 p95 **33.45 ms**。
- GPU busy **96.91%**，current 均值 680 MHz；8.038 秒 perf 内热 guest TID 14110 的
  surface capabilities 查询 off-CPU **0.311 秒**。
- 1322 个完整 trace、0 顺序错误，另 2 个边界不完整帧保留；simpleperf 307700 samples、0 lost。
- 应用 PID 9477、MHR PID 13825、wineserver PID 13706；start-time ticks 分别
  143480224、143523734、143523601。设备 build/boot 与 A/B/A 相同。
- 设备已安装 APK SHA 与本地审计一致；逐库查询确认上述 native Build IDs。
  批量 modules 达到 256 项上限的截断结果也保留，未拿它当完整模块清单。
- 结束时 19815 次异步完成、1 次跳过、0 失败、0 在途；包含启动/菜单，不能全部当作场景帧。
  Present trace 已停止，profiler capture_active=0 / ready、recording_active=0，DebugBus 服务已停止。
  设备留在暂停画面，点击中央播放按钮可继续，MHR 异步配置保持开启。

这组是 UI 包的独立复核，不与旧包拼成同包 A/B；没有观察到本场景性能回退或明显花屏。
更多游戏、完整混合 Present 请求、长时间游玩及设备丢失仍未覆盖。

## 身份与证据

- A/B/A APK SHA-256：`6f485c301e23b0f2d21f13c38f2409ddbb1c54b37e33e84e539ea9f516c8d383`。
- UI/暂停修复 APK SHA-256：`053ed5ea044a33b185db1ec0761bee79d09a04e5876cb31e0c7d57c71be3a471`。
  清空旧 APK 后构建；APK 审计 123 项、0 错误，保留数据安装。
- 两包 catalog SHA-256：`e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`。
- `.so` Build IDs：renderer `6a799a63e3b40275cf373d803df361cecb183482`；
  winlator `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  evshim `5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`；
  debugbus `30a9c69fe7e2e2c13006718a8591f3d8619cdc23`。
- 设备 AYN Thor，Android 13 / API 33，targetSdk 36，4 KiB；build
  `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；
  boot id `8bb14501-5800-4b9b-a9ab-7173603812f7`。
- A/B/A 应用 PID 17019，MHR PID 30774，wineserver PID 30656。各段采集 30 秒，
  负载差分窗口 29 秒；perf 8.048 / 8.049 / 8.050 秒；start-time/maps 前后记录一致。
- 私有原始证据（含失败/未完成尝试、ANR、截图与配置）位于仓库外
  `xrgame-native-evidence/mhr/20260929/async-present/`；不提交角色、账号、游戏数据或 APK。

历史启动媒体黑屏见 [此前记录](mhr-black-screen-20260928.md)。这次同步路径已能进入据点，
异步呈现解决的是场景等待瓶颈，不能归因为修复了历史媒体加载问题。启动时仍出现既有
MR-111-81 联机错误，关闭后可继续离线流程；本次不代表联机功能验收。
