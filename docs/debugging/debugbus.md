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
