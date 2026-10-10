# Swan XR：DebugBus 自动化、快捷菜单修正与头显性能 HUD（2026-10-09）

设备 Swan（Pico B3110，Android 16，Adreno 840），构建 `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0`。
游戏 Half-Life: Alyx（OpenComposite → 本仓 Windows OpenXR runtime → 头显 OpenXR，Vulkan 合成 + SGSR + 眼动 FDM）。

**结论**：
- 新的 DebugBus 命令可以从主机启动 Alyx、注入手柄输入、开关并导航快捷菜单、开关 HUD，四轮会话都只靠这些命令进入游戏并操作。
- 快捷菜单在 OpenXR 下变形的原因已修正：Pico 给沉浸式 Activity 的是 1080x1920 竖屏窗口，UI 被画成竖屏位图再拉到 16:9 面板上。现在 UI 按 1920x1080 横屏布局。
- 头显性能 HUD 按 shadPS4 的做法实现，在立体模式下显示并持续刷新。
- 合成器 GPU page fault 有了新线索（见第 5 节），未修。

## 1. DebugBus 自动化命令

命令说明见 [DebugBus 文档](../debugging/debugbus.md#沉浸式自动化2026-10-09)。

| 命令 | 实现 | 设备结果 |
|---|---|---|
| `launch <appId>` / `launch status` | 向 `MainActivity` 发 `app.gamenative.LAUNCH_GAME`，与点「开始游戏」同路；有游戏在运行时拒绝 | run38–41 各一次 `launch 546560`，约 90 s 进入立体模式 |
| `input btn/axis/release/status` | 原生 `syncControllerInputs` 中与真实手柄合并，带过期时间 | `input btn a` 在暂停页按下「继续」，游戏恢复 |
| `quickmenu open/close/toggle/nav/status` | 主线程单次调度，最多等 1.5 s；`nav` 走 `input` | run41：`nav down` → `DPAD_DOWN(20)`，`nav up` → `DPAD_UP(19)` |
| `vr_hud on/off/status` | 改快捷菜单的「性能 HUD」开关并通知原生 | 返回图层是否创建、重绘次数、耗时与当前 FPS |

**说明**
- 打开快捷菜单时主线程会卡 1–1.6 s（`Choreographer: Skipped 73–107 frames`），所以 `toggled` 常为 null（未在等待时间内执行完），之后 `status` 能看到菜单已打开。
- 快捷菜单把快照里的 `ly > 0` 当作向下，而游戏原样把同一个值当作 XInput Y 收到，两者约定相反。DebugBus 已按菜单约定发送；物理摇杆在 Swan 上导航菜单的方向需要人工确认。

## 2. 快捷菜单（侧边面板）

**原因**（run37 及以前的 logcat）：`overlay layer refreshed size=1080x1920`。快捷菜单按 432 dp 宽的 COMPACT 档布局，侧面板占约 85% 宽度，再被全屏拉伸到横屏 quad 上，水平方向拉宽约 3 倍。

**修改**（`ImmersiveXrActivity`）
- 配置与 `LocalConfiguration` 改为横屏（屏幕 768x432 dp，密度不变），Compose 根视图固定为 1920x1080 px，overlay 位图与指针命中都按这个视图计算。
- 停止平面呈现（进入立体模式）时，原生侧丢弃保存的平面游戏 AHB。此前 Vulkan/GLES 合成会把最后一帧平面画面不透明地垫在菜单底下。
- 菜单或暂停页打开期间，Windows VR 游戏只收到手柄姿态，不再收到按键、扳机和摇杆；关闭后等按键松开才恢复。此前在菜单里按 A，游戏也会收到 A。

**设备结果**
- run38–41：`overlay=1920x1080`。
- run38 的头显截图里，菜单是左侧约一半宽度的面板，比例正常（`run38-automation-hud/quickmenu-open-1.jpg`）。

**开菜单后场景变黑**
- 默认挂起策略下，开菜单会 SIGSTOP 游戏，之后头显里的游戏场景是黑的。
- 关菜单并「继续」后，场景会自己恢复，不改任何设置。run39 在 50 s 内恢复。
- run38 中还观察到：一直不恢复时，切换 `vr_upscale` 后立即恢复。
- 原因（Pico 对静止帧的处理，或 Alyx 自身对长时间卡顿的处理）未确认。容器挂起策略设为「从不」可以避免游戏被冻结，此次没有改用户配置。

**截图限制**：Pico 系统截图有时不包含本应用的 quad 层，而同时 `dumpsys xrtruntime` 显示层确实已提交（run38：Layer1 QUAD 1536x192，1.6x0.2 m）。因此截图里没有某层，不能直接当作该层未显示。

## 3. 头显性能 HUD

**实现**：`xr_perf_hud.{h,cpp}` 是本仓代码，链接固定的 Foundation 子仓，不复制其源码。
- 独立 ImGui context，由 Foundation `XrImguiVulkanLayer` 画进单独的 OpenXR quad，1.6x0.2 m，画布 1536x192。
  - 2026-10-09 版放在 VIEW 空间（跟头），距眼 2.5 m，左上角。
  - 2026-10-10 按用户要求改为固定位姿：面板固定在 LOCAL 空间，不跟头。HUD 每次打开（以及 LOCAL 重新居中后），用一次 `xrLocateSpace` 取当时的头部位姿，放在偏航方向前方 2.5 m、左 0.9 m、上 1.0 m，面板朝向头部。之后每帧直接复用这个位姿，不增加等待。
- 每秒最多重绘 4 次，非阻塞；首次绘制前烘焙全部字形。
- 游戏 FPS 只统计带来新游戏图像的投影帧。
- 设备指标用 Foundation `DeviceMetricsReader`，跑在自己的 1 s 线程上，并显式接 Android BatteryManager。
- 只在 Vulkan 合成后端提供。显示跟随快捷菜单的「性能 HUD」开关。

**Swan 上发现并在 HUD 侧绕开的两个问题**
- `cpu-hw-trip-0/1` 温区恒读 105000，是触发阈值而不是温度。Foundation 取所有含 "cpu" 温区的最大值，于是 run38 显示 CPU 105 °C。HUD 的文件源隐藏类型含 "trip" 的温区，run39 起显示 58 °C（同时刻 `cpu-virt-max` 约 64 °C）。
- Foundation 采样器每秒重读被 SELinux 拒绝的 `kgsl gpuclk/clock_mhz`、`gpu_clock` 和 `devfreq`。run38 中 HUD 显示的约 14 分钟里产生约 3000 条 avc。HUD 的文件源对 `/sys` 下读失败的节点不再重试，run39–41 中这类拒绝为 0。
- 根本修正应在 Foundation `perf_metrics` 中完成（需要另开 Foundation 分支），本次未改。

**设备结果**
- run38：`updates=774`，单次重绘最长约 2.1 ms。
- run41：`updates=286`，`maxUpdateUs=2930`。
- 游戏 FPS 观测值 18.7–38.5，XR 72/72。
- 截图见 `run38-automation-hud/quickmenu-open-1.jpg` 和 `run39-hud-sampler/quickmenu-open.jpg`。
- run38 中 HUD 开/关各 25 s，page fault 都处于内核限流上限（见下节），HUD 不是来源。
- run53（固定位姿版，APK `c52cc33afafe2eaa607f154db323794cb25e2111ebc533d294e6b998509268be`，Turnip xrg13）：
  - 面板放在 LOCAL 中的 (-0.93, 0.87, -2.47)；`vr_hud off/on` 后重新放置。
  - 头显截图中面板固定在场景里（`run53-hud-world/hud-world-1.png`）。设备平放有侧倾，所以截图里面板与场景同样倾斜。
  - 重绘 156–272 µs，`reused=0`。
  - 游戏 FPS 23–24，与改动前同场景的 23–24.3 相同，XR 72/72。
  - 本进程 page fault 为 0。
  - 用户尚未在头显内确认位置。

## 4. HUD 温度读取（Kotlin 侧）

`SystemMetricsReader` 对打不开的温度节点不再重试。run39–41 中 kgsl `temp` 的 avc 为 0；单元测试 `SystemMetricsSourcesTest` 2/2 通过。

## 5. 合成器 GPU page fault（新线索，未修）

> 2026-10-10 更新：根因已定位并用诊断 Turnip 验证。Turnip 在 A840 上从不初始化 bin foveation 寄存器，其他 GPU 上下文留下的值会被继承。见[根因记录](swan-xr-composite-faults-20261010.md)。下文的「按输出尺寸写进源尺寸图像」推测不成立。

- 每轮都有，run38–41 分别为 1981/301/574/209 条（受内核限流，每 5 s 最多 10 条）。全部在合成器上下文（drawctxt 51）。没有 `GMU_GPU_HW_HANG(601)`。
- run38 A/B（每档 20 s）：

| 配置 | fault 数 |
|---|---|
| `filter=off` | 0 |
| SGSR + `fov=off` | 34 |
| SGSR + `fov=fixed` | 40 |
| SGSR + `fov=eye` | 34–40 |
| FSR1 + `fov=off` | 40 |
| SGSR + `out=75` | 40 |

  `fov=off` 期间 FDM 计数不增长，所以与 FDM 无关，只与重建 pass 有关。
- 用 `xraddr` 地址绑定日志对照：CCU 写越界的地址都不在任何已绑定内存内。对槽 0 和槽 1，越界地址正好都是各自 1296x1200 中间图像（`prepared`）基址 + `0xD17900`。
- 这几轮 Alyx 的眼图为 1296x1200，是输出 2592x2400 的一半（GPU 99% 时 Alyx 自动降了分辨率）。run35 的源尺寸更大，当时只有零星 fault。推测某个 pass 按输出尺寸写进了源尺寸的图像，或地址计算以该图像为基准越界。具体是哪个 draw，需要 RD 抓帧或 fault 快照确认。
- 越界写落在地址空洞时产生 fault；如果落在其他分配上，会静默破坏数据。

## 构建与证据

| 轮次 | APK SHA-256 | `libxrimmersive.so` Build ID | 内容 |
|---|---|---|---|
| run38 | `4cbf99f16fb2546d9633ffc3d7fc84553c9f482729cab7373c8dca6b1978b7c7` | `b33de6eacfc754a8e7897cba8a64c57f37e7b897` | 首轮：自动化、横屏菜单、HUD |
| run39 | `2fdf0a4faa9eaf887416ddd62fd7def9620c0c39f3d1690e59cf5df2f2873249` | `8188f96a23fef274afb556bcd977ba85a704db6a` | HUD 采样修正、quickmenu 单次调度 |
| run40 | `c13cbc36353eb9cfcf384868a784f97f8846b82d45deef1c64ebaf12f0e424b2` | `8aa49baddea3adc61fc1538c6f69314fffa88689` | 菜单期间屏蔽游戏输入 |
| run41 | `633fb43e42401578e4a4b46926eda363a15f411373e7b33264ea5d4dad7ea802` | `a3381f716c4d17d9f7c380386a463bbb26d70076` | 最终包：菜单导航方向 |

- 组件 manifest 均为 `c59343a031a310ebb8319ecdfe0430f34b46c0f28a990b21a536564d3a55f975`（Turnip xrg12）。audit-apk 0 错误。
- run41 中 app PID 4435：22:17:36 创建 HUD 图层，22:18:50 进入立体模式，持续约 3 分钟。
- 单元测试：`XrDebugInputTest` 4/4，`SystemMetricsSourcesTest` 2/2，`WindowsVrGripCorrectionTest` 3/3。
- `debug.xrgame.xr.vktrace=1` 全程开启。
- 证据（logcat、截图、APK、审计、`dumpsys xrtruntime`）保存在仓库外的 `xrgame-native-evidence/swan-20261008/swanxr-ahb/run38-automation-hud` 至 `run41-final`。
