# Foundation DebugBus

用户于 2026-09-27 授权引入私有 Foundation 子仓。正式构建依赖位于根目录 `foundation/`，
来源 `tencentmalos/foundation` 的共享 `main`，初始 gitlink 为
`d487242023e6899f680717d263cf35c7c1f9e6c2`，与 shadPS4 当日已提交版本一致。
不复制 shadPS4 的本地修改，也不初始化 Foundation 的 `reference/` 研究仓库。
2026-09-28 的阶段 gitlink 为 `c1d339c3a9b231ad23d02e4fd982e3e7120f7ef5`：
独立分支 `feature/malos/xrgame-litep-startup` 已 rebase 到共享 `main` 的
`9b29d357` 并推送，加入启动阶段 profiler ring 与 DebugBus 命令。

```sh
git submodule update --init foundation
```

仅内部 `picoXrDebug` 构建 `libxrgame_debugbus.so` 和 `DebugBusService`。CMake 单独加入
`foundation/modules/debugbus`；后续 Litep 接入还单独链接 profiler ring 与其 SDK/LZ4
依赖。没有接入音频、手柄或整个引擎。Foundation 当前未声明顶层公开分发许可证；此接入
用于用户授权的本地研究，不能作为公开发布 Foundation 源码或二进制的依据。公共 CI 若要
构建内部调试包，需要单独配置只读私仓访问，以及既有的完整运行时 bundle；本次未配置 CI 凭据。

## 使用

先正常打开 XRGame Native，再启动诊断服务（不会自动启动游戏或更改配置）：

```sh
python3 tools/xrgame/debugbus.py --serial <device> --start bridge
python3 tools/xrgame/debugbus.py --serial <device> help
python3 tools/xrgame/debugbus.py --serial <device> status
python3 tools/xrgame/debugbus.py --serial <device> runtime
python3 tools/xrgame/debugbus.py --serial <device> processes
python3 tools/xrgame/debugbus.py --serial <device> modules
python3 tools/xrgame/debugbus.py --serial <device> modules libxrgame_debugbus.so
python3 tools/xrgame/debugbus.py --serial <device> present
python3 tools/xrgame/debugbus.py --serial <device> present trace 120
python3 tools/xrgame/debugbus.py --serial <device> present trace 0
python3 tools/xrgame/debugbus.py --serial <device> present async_copy 1
python3 tools/xrgame/debugbus.py --serial <device> present async_copy 0
python3 tools/xrgame/debugbus.py --serial <device> present copy_pipeline 1
python3 tools/xrgame/debugbus.py --serial <device> present copy_pipeline 0
python3 tools/xrgame/debugbus.py --serial <device> --stop
```

原始入口是 `adb -s <device> shell dumpsys activity service
com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService <command>`。
服务由 `android.permission.DUMP` 保护，Android dumpsys 本身要求 shell/系统权限；不监听 TCP，
无需端口转发。进程重启后需再次 `--start`；服务不做 sticky 后台重启。

| 命令 | 返回内容与边界 |
| --- | --- |
| `bridge` | Foundation 固定提交、宿主 PID、transport、scope |
| `status` | 宿主 PID/UID、系统与目标 API、设备 build/boot id、服务起始时刻、活跃 Steam App ID、X server 是否存在 |
| `runtime` | APK catalog SHA、默认组件版本、安装回执是否对应 catalog；**不重新哈希大文件，不证明已装文件完整或该进程实际加载了全部默认项** |
| `processes` | 当前 UID 可见进程的 PID/PPID、comm、state、start-time ticks；进程退出竞争会计入 skipped，超量/超时标 truncated |
| `modules [exact-basename]` | 当前 Android host 已加载 ELF 的名称、基址、GNU Build ID；超过 256 个时显式标记 truncated，可按完整文件名查询；不是 Wine PE/FEX guest 模块表 |
| `present` | 当前 host Present 的待归还队列、CPU pacing 队列、限帧、trace 余量；近似并发快照，不获取 GPU 锁 |
| `present trace N` | 当前会话最多记录 N 帧（0–3600），0 停止；仍通过 `XRGamePresentTrace`/原生 present trace 输出 logcat |
| `present async_copy 0/1` | 当前会话的 AHB GPU copy 对照开关，初始关闭；在已接收任务归还后切换。查看 `asyncCopyRequested/Active`、`outstandingCopies` 和完成/跳过/失败计数 |
| `present copy_pipeline 0/1` | 实验性两阶段 copy，默认关闭、仅当前会话；需 `asyncCopyActive=true`。同队列的 GPU barrier 保证 copy→draw 顺序，独立线程等待 copy fence 后才通知 Complete/Idle。查看 `copyPipelineRequested/Active`；模式切换等待已接收任务归还 |
| `instrumentation [off\|coarse\|detail]` | host Litep 桩点层级；默认 coarse，仍需显式开始采集；off 不结束已开始的采集会话 |
| `profiler_ring [status\|start\|stop\|dump N]` | 每线程 1 MiB 的有界 ring；dump 0 允许零帧启动，等待准确 dump_id 完成后收集 |
| `profiler_capture [status\|stop\|file MiB seconds]` | 有界 Streaming 文件，完成后恢复此前 ring 状态；保留 PROF 与 sidecar |

Litep 的主流程覆盖、开销控制、跨线程/截断语义和采集示例见 [Litep 桩点说明](litep.md)。

`async_copy` 首轮只覆盖无 wait/idle X fence、无偏移、未启用 Present 限帧的 AHB copy；
直接采样及其他路径不纳入这次优化。picoXr 可在游戏详情 → 编辑容器 → 图形中切换
**异步呈现（实验性）**，保存并重启游戏后生效；默认关闭。需要 Vulkan、DRI3，
并关闭快捷菜单 FPS 限制器，游戏内限帧可保留。配置沿用容器环境变量
`XRGAME_PRESENT_ASYNC_COPY=1`；UI 关闭会写 `0`，已有 opt-in 可直接读回。
运行中启用快捷菜单限帧会在已接收 copy 归还后切回同步，关闭限帧则恢复用户的异步选择。
`requestedFrameRateLimit` 为请求值，`frameRateLimit` 为当前生效值。
`copy_pipeline` 不改变游戏 VSync、DXGI 最大在途帧数或源 AHB 归还时机，也不启用直接采样。
它移除 renderer 在 copy 与 draw 之间的 CPU fence 等待；命令池释放仍在 renderer 上，
回收线程只等待 fence 并发出完成通知。队列容量包含已提交但尚未完成的 copy；
窗口取消不能提前归还在途 AHB，GPU 等待失败时保留资源到设备销毁，不发 Idle。
Litep 的 `host.present.copy` 此时是提交阶段，`host.present.retire_wait` 是回收线程的等待；
跨线程 `host.present.queue_to_complete` 仍覆盖完整请求寿命，不能把两个阶段的并行时间相加。
验证与限制见 [MHW](../validation/mhw-present-async-20260928.md) 和
[MHR / UI 验证](../validation/mhr-present-async-20260929.md)。

JSON 返回 `schema: 1`。Kotlin provider 的 `sampledAtBootNs` 使用 Android elapsed realtime
（含休眠）；现有 Present trace `mono_ns` 使用 `System.nanoTime`（CLOCK_MONOTONIC），两者
不可直接相减。`startTimeTicks` 是 Linux `/proc/PID/stat` 字段 22；结合 boot id 使用，不能
只凭 PID 认定调试对象身份。查询不返回 cmdline、环境变量、Steam 账号/票据或任意文件内容。

## 接入边界

```
ADB dumpsys → DebugBusService → JNI → Foundation DebugCommandRegistry
                                      ├─ bridge / host ELF inventory
                                      └─ host providers: runtime / process / Present
```

Foundation 管理命令分发。Android Service 承担 dumpsys transport，因此无须另绑定进程全局
`DumpsysBridge` 注册表；每个请求的注册表/回调只活到 JNI 返回，没有持久 JNI 引用。
单 worker、一个排队槽、2 秒等待上限；进程枚举另有 1 秒预算、128 进程上限，ELF 模块上限
256，回复上限 128 KiB。未知命令返回帮助，非法参数明确报错。查询超时不销毁正使用的注册表，
后续查询可返回 busy；它不保证强行中断系统调用。停止服务释放 worker，不持有 Activity/renderer。

**合适的后续扩展点：**

- X server/renderer：在现有 Present 和 AHB lease 所有者处发布有界状态，供 provider 读取；
  不让 dumpsys 在等待 GPU 的 renderer 锁上卡住。
- Wine/FEX：是独立 Linux 进程。需要其自有诊断端点和 session/PID/start-time 身份校验，
  再由 host 汇总；本次没有接通 guest 寄存器、断点或内存读写。调试控制仍走 native/guest debugger。
- DXVK：在 `CreateBuffer`/资源创建失败处记录描述符、HRESULT、guest 模块信息，由 guest 端点
  汇总。当前 MHW 的 `CreateBuffer(E_INVALIDARG)` 是直接适用的下一项；不能用 host ELF 列表
  推断 guest 的 DXVK 调用参数。

实机证据见 [首次接入验收](../validation/debugbus-20260927.md)。
