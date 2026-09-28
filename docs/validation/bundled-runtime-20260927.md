# 默认随包运行库与自动修复（2026-09-27）

用户要求把 Steam、DXVK、D3D10/11 配套组件作为默认随包能力，减少启动时缺 DLL。
原 APK 已包含这些压缩包，但 Steam 客户端只为个别游戏启用，损坏的组件缓存仅报错退出。
本轮修复这两个缺口。游戏兼容性仍需逐项验证，不能由“DLL 齐全”推导可玩。

## 实现

- picoXr APK 必须内置 Proton、FEX、DXVK、VKD3D、Turnip、imagefs 与源码 Steam 客户端。
  Gradle、打包准备与独立 APK 审计检查默认版本、文件存在性和 SHA；缺项停止出包。
- x64 Steam 游戏根据实际 PE 架构默认配置客户端，保留原 EXE、工作目录和参数；
  专用 profile 只负责路径覆盖及可选 extra 钩子。32 位 EXE 不注入 x64 客户端。
- 已安装组件树每次启动校验；缺失/损坏时从认证包重新安装，失败回滚，进程中断留下的
  临时备份可在下次恢复。有 Wine 进程运行时拒绝替换共享运行库。
- 每个 prefix 校验 DXVK 的 `d3d8`、`d3d9`、`d3d10core`、`d3d11`、`dxgi`，
  VKD3D 的 `d3d12`、`d3d12core`，以及 Proton 的 `d3d10`、`d3d10_1`、
  `d3dcompiler_47`；写入使用临时文件、校验及原子替换，并配置相应 DLL override。
  不通过删除 prefix、替换游戏 DLL 或清空用户目录修复。

## 已通过验证

26 项 Kotlin/JVM 测试通过，包括安装损坏、复制失败不破坏现有 DLL、失败回滚、
进程中断恢复、运行中拒绝修复、PE 架构分流及参数处理。7 项归档测试通过，
7 个真实组件归档按加强后的完整性规则通过；APK 审计 122 项、0 错误。

AYN Thor 自有 WindowsProbe 容器的故障注入：

1. 备份后将安装缓存 `d3d11.dll` 改为测试文本，移除 prefix 的 `d3d11.dll` 和 `d3d10.dll`。
2. 启动日志在 11:24:42.517 记录从 **APK 内置归档** 安装 DXVK xrg2。
3. 缓存与 prefix 的 D3D11 均恢复为
   `95fbdd6812c1996ecde83de6d2de32cdf245b480a2cdbc6eea6a6f1f892dfc82`；
   D3D10 恢复为 `e4a476103bdc635c778e5cf4339aa2c4e9100b60b3c4b2d92c271f8b15d851ed`。
4. fixture 的 D3D11 device/shader/resize 均返回 `S_OK`，完成 first Present、显示蓝色清屏；
   主动正常退出，报告 `elapsedMs=77865`、`frames=4671`、`result=0`。
   这是 fixture 自报值，不是游戏性能基准。
5. 新增 `--d3d10-check` fixture：D3D10 与 D3D10.1 hardware-device creation 均返回
   `00000000`。此项验证创建接口，不代表 D3D10 游戏场景验收。

故障注入只涉及我们的运行库缓存与自有测试容器；测试后恢复 fixture 原 EXE 与 `--gdi` 配置。
测试没有清空 app 数据、账号、游戏目录或存档，也没有关闭设备网络。

## 证据身份

- 自动修复 APK：`f10e6e926f314d80c8ed3270f1512bda2a04b57e2c46b19ab798c9a796e9b057`。
- 自动修复 catalog：`90eaac4a0bb18ecaa1d517f0ea2a3a7544e486efc2cd9cc8114b08a63de056e2`。
- D3D10/10.1 API 测试 APK：`718837dfd39c8ea53f7893ec9f9c99c77fface730a5c6b444e915cf6788e5180`；
  catalog：`6f2d7d951eadcb04c59ace83ad1d4bacd3fbb5d127fcd21fc0c2e4588858e137`。
  与前一包的 Proton/DXVK/VKD3D 完全相同，仅加载器验证版本变化。
- `libwinlator.so` Build ID `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  `libvulkan_renderer.so` `36421bccc5e74ef19b0ff30b47b21ea071507eab`；
  `libgndownload.so` `674e470eca24f48e67e09f5329fc7e7a58b98469`。
- 设备 AYN Thor / Android 13，build
  `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`；
  boot `8bb14501-5800-4b9b-a9ab-7173603812f7`。
- 修复验证 app PID `6012`，guest Unix PID `7072` / Wine PID `240`；
  D3D10 API 验证 guest Unix PID `15467` / Wine PID `236`，短进程未取 start-time ticks。

私有原始记录位于仓库外 `xrgame-native-evidence/bundled-runtime/20260927/`：
`repair/` 保留故障前文件、哈希、截图、日志、进程身份及 fixture 报告；`d3d10/` 保留 API 报告；
测试 XML、打包/编译日志、源码与二进制哈希、APK 审计均保留。失败的 MHW 对照与加载器候选
也保留，见 [MHW 专项记录](mhw-startup-20260927.md)。未提交、推送或公开发布。

## 范围

本轮默认运行时针对 Windows x64 / ARM64EC；没有声称提供完整 32 位 DXVK / Steam 客户端，
也没有随意加入 Microsoft 专有 VC++ 安装包。Proton 自带实现继续作为基础依赖。
MHW 的特殊 Steam 启动兼容问题独立追踪，不能把本轮 D3D 接口与自动修复通过写成 MHW 已可玩。

## 后续 loader 与 DebugBus 复测

Steam 客户端候选继续迭代到 validation7，解决跨进程 ARM64EC 导入 thunk 地址不可复用的
注入故障；该轮 MHW 越过 Steam 初始化后仍遇到 D3D11 CreateBuffer。后续配置路径修复见
[MHW 记录](mhw-startup-20260927.md)。包含 validation7 客户端的最终 DebugBus 调试包
在 AYN 再次显示 Hades II 主菜单，正常退出；完整 APK/catalog/Build ID 与 PID 见
[DebugBus 验收](debugbus-20260927.md)，不将主菜单回归扩大为整游戏验收。
