# Swan XR：Vulkan 合成（SGSR/FSR1 + ETFR）、Turnip xrg12 与手柄射线修正（2026-10-09）

设备 Swan（Pico B3110，Android 16，Adreno 840），构建 `Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006`，boot id `9cbe019b-9d6f-4a49-8a61-062e11b746e0`。
游戏 Half-Life: Alyx（OpenComposite → 本仓 Windows OpenXR runtime → 头显 OpenXR）。

**结论**：
- Vulkan 合成路径可以用超分加眼动 FDM 显示 Alyx。
- xrg12 在完整 LRZ 下跑了一次约 19 分钟，没有出现硬件卡死。这只是一次运行，不算长时间稳定性验收。
- 合成器仍有 GPU page fault 未修。
- 手柄修正值是按几何推算的，还没有在头显里确认对齐。

## 1. Turnip xrg12：A8XX 默认关闭 LRZ fast clear

**改动**
- mesa-mirror `malos/main` `04e1d665`：芯片代际 ≥ 8 时，主 render pass、secondary 和 `vkCmdClearDepthStencilImage` 都不再使用 LRZ fast clear。`TU_DEBUG=lrzfc` 可以恢复原行为。
- 2026-10-09 `malos/main` 从 `d15b7c01` 快进到 `04e1d665`，同时发布了此前只在本地的 `25ef1647`（shadPS4 与 Azahar 的合并）。
- `feature/malos/xrgame-wine-icd` 合入后为 `57e87576`，本仓 gitlink 指向它。
- 配方 `build-turnip.sh` 的 pin 改为 `04e1d665`，补丁仍为原来 3 个。构建源码树与 `57e87576` 一致。

**依据**
- run30（xrg11，完整 LRZ）约 80 秒卡死，快照 devcd40。GMU 记录 `GMU_GPU_HW_HANG(601)` 和 MISHANGDETECT。
- 卡死时 BR 停在开启 LRZ fast clear 的 pass 里，有 3 个 WFI 和 2 个 cache-clean 未完成。
- 同代 GPU 的 13 份高通官方驱动快照里从未出现 `LRZ_CLEAR`。

**产物**

| 项目 | 值 |
|---|---|
| `turnip-04e1d66-xrg12.zip` | SHA-256 `514c9f4a0823e2a14126d4e51e29ac31cc91470763168683a82ed8d48cbf0aab` |
| Wine/X11 `libvulkan_freedreno.so` | Build ID `2aba1aa5cff36b74912974568e0eaa265b64f54d`，打包后 SHA `b1a9db56…` |
| app 进程 `libvulkan_freedreno_android.so` | Build ID `e07c8618fe7f2dbdcd414516c17214fd02101854`，打包后 SHA `c3889a74…` |
| 组件 catalog / manifest | `c59343a031a310ebb8319ecdfe0430f34b46c0f28a990b21a536564d3a55f975` |

**设备运行（run31）**
- APK `90dfeb2ff4b5900aa4f677c6f4562ba7c7090bb650560c2e4b5388383e33c9e6`。
- 容器 `TU_DEBUG=noconform`（完整 LRZ），配置与 run30 相同，合成走 Vulkan。
- 设备上两个 Turnip 库的 SHA 与包内一致。
- 会话 1：hlvr.exe PID 24403，约 3.5 分钟，用户通过系统界面退出。
- 会话 2：app PID 20187，hlvr.exe PID 26697，19:06:19–19:25:36，约 19 分钟。合成投影 35400 帧（72 Hz），游戏自身约 30 fps。
- 结果：0 次 `GMU_GPU_HW_HANG(601)`，hlvr.exe 无 page fault。
- devcd42（19:08:51）是 Swan KGSL 自带的 hwsched 无进展 watchdog：`picoxr.systemui` 低优先级上下文在 GPU 99% 忙时 2.5 秒没有推进。GMU 日志里没有 601。用户确认这是自加的强杀机制，不作为卡死分析。

## 2. Vulkan 合成：SGSR/FSR1 超分与眼动 FDM（ETFR）

**链路**
- app 进程通过 adrenotools 加载 xrg12 的 Android 版 Turnip，用 `XR_KHR_vulkan_enable2` 交给 Pico runtime。
- 游戏眼图以 AHB 导入（3670x1699，单采样），由 Foundation 的 `VulkanUpscaler` 重建到 runtime 推荐的 2592x2400。
- 重建 pass 使用 FDM，注视点来自 `XR_EXT_eye_gaze_interaction`。

**设置入口**
- 每个游戏的设置里，VR OpenXR 模式下有「VR 合成」一组。值保存在容器环境变量 `XRGAME_XR_COMPOSITE`、`XRGAME_XR_UPSCALE`、`XRGAME_XR_SHARPNESS`、`XRGAME_XR_FOVEATION`、`XRGAME_XR_FOVEATION_LEVEL`、`XRGAME_XR_OUTPUT_PERCENT` 中。
- 默认后端仍为 GLES。
- DebugBus `vr_upscale` 可在运行时切换。
- XRGAME 构建打开游戏设置时，先进入只含 XR 选项的精简页，原有全部标签页收在「Legacy 设置…」里。

**设备运行（run35）**
- APK `54ef581ef8c8448799b23ab59c0ccc4419d36f1ba362d1b726975bf102cbb79f`。
- 容器保存了 `vulkan / sgsr / 50 / eye / balanced / 100`。
- 至 3000 帧时统计为 `filter=2 fov=2 level=1 gaze=1 draws=6000 fdm=6000 tracked=6000 uploads=5867 unchanged=133`，即每次超分都用了 FDM 并跟随注视点。
- 眼动权限已授予。会话开始约 0.7 秒后拿到第一个有效注视点。Pico 平台眼动模式的 hook 返回 `-1000710000`，但标准扩展照常提供数据。

**FDM 与 MSAA**
- 合成器里的所有图像都是单采样（trace 证实），不受 Turnip「多采样附件需要 load/store 时关闭 FDM」这条规则影响（`tu_pass.cc` `tu_render_pass_disable_fdm`）。
- 游戏在 Wine 侧渲染，不使用 FDM。在 devcd40 和 devcd42 中，Alyx 的命令流里有 GRAS/RB 的 4x MSAA 设置，说明游戏自己的 MSAA 正常生效。具体是哪些 pass 用了 4x，需要对游戏抓帧才能确定。

**已知问题，暂缓（用户决定）**

合成器 Turnip 上下文（drawctxt 51）有 GPU page fault：
- 每次会话开始：读 3 张 1280x720 quad swapchain 图像各自末尾之后 0xFBC0–0x10380 字节的 UBWC flag，属于 Flag cache 读越界。
  - Turnip 报告这些图像需要 0x38A000 字节，runtime 的实际绑定与此一致。
  - 越界只出现在每张图首次渲染时。渲染 pass 是 `Context::blit`（CLEAR/STORE，render area 1280x720）。
  - 下一步：用 RD 抓前几次提交，核对 flag 基址。
- 运行中偶发 CCU 写越界，地址落在分配之间的空洞。两次都发生在系统指示器 `SystemUIImmersiveIndicator` 弹出之后，第三次弹出时没有复现，期间我们没有释放内存。
- 调试开关 `debug.xrgame.xr.vktrace=1` 会输出 `xrvktrace`（我们的 Vulkan 调用和各库的入口查询）和 `xraddr`（`VK_EXT_device_address_binding_report` 的 GPU 地址），可以把 fault 地址对应到具体对象。
- 另外，Pico runtime（`libpxrruntime.so`）只通过我们给的入口查询了 `vkCreateInstance` 和 `vkCreateDevice`，swapchain 图像不经过这个入口创建。
- 2026-10-09 晚：Alyx 眼图降到 1296x1200 后，CCU 写越界变为持续出现，只在开启重建时发生，地址按超分槽位固定。见[自动化与 HUD 记录](swan-xr-automation-hud-20261009.md#5-合成器-gpu-page-fault新线索未修)。
- 2026-10-10：根因是 Turnip 在 A840 上不初始化 bin foveation 寄存器，继承了其他上下文留下的值。复位后 fault 为 0，Turnip 主干改动待合入。见[根因记录](swan-xr-composite-faults-20261010.md)。

## 3. Windows 游戏手柄射线修正

**原因**：Windows 侧把手柄统一报告为 Oculus Touch。OpenComposite 用 Quest 2 模型的 `openxr_grip` 变换（绕 X 轴 20.6°）把 grip 换算成 OpenVR raw 姿态；Alyx 只绑定 `pose/raw`，再按自己内部的 Touch 偏移算射线。

**实测**
- Swan 的 Pico runtime 报告 `bytedance/pico4_controller`。
- 左右手的 aim 都在 grip 下方 34.0°，平移 (-1.6, -39.2, -32.8) mm，两次会话结果相同。
- 用户观察到，未修正时游戏射线比手柄平面低约 30°。按 Quest 偏移推算，偏差应为 24°（34° 对 58°）。

**实现**
- 只对发给 Windows 游戏的 grip 叠加刚体修正（右手坐标系，左手镜像）。quad 指针不受影响。
- `WindowsVrGripCorrection` 按 `Build.DEVICE` 选默认值，`swan` 为 pitch +24°。
- DebugBus `vr_grip` 可以实时调整。会话开始时日志会打出 runtime 的 grip→aim 关系。

**状态**：+24° 已在运行中的会话里下发，用户尚未确认对齐效果。

## 构建

| 用途 | APK SHA-256 | 说明 |
|---|---|---|
| 最终包 | `2227ac846103c3e39f86beb62cf3b9f241752a1ba55836e6a24148e7602e34fd` | `libxrimmersive.so` Build ID `540808a9821271139bd4f444528dad3aad5b48b3`，manifest `c59343a0…`，audit-apk 0 错误；未在设备上安装 |

单元测试：
- `WindowsVrGripCorrectionTest` 3/3；
- `WindowsVrUpscaleTest` 4/4；
- `XrGameRuntimeTest` 5/5。

证据（logcat、快照、APK、审计）保存在仓库外的 `xrgame-native-evidence/swan-20261008/swanxr-ahb/run30-vulkan` 至 `run37-grip-default`。
