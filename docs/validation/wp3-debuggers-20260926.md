# W3 native / guest 调试验证（2026-09-26）

**基础调试通过；本文记录的是黑屏定位前的调试轮次。** 后续 Hades II 已通过 AHB GPU
拷贝路径进入场景，见 [后续修复](wp3-hades2-20260926.md)。完整 mixed-stack、单步和断点仍未验收，
下一步见 [原 spec 对照与 D0–D4](spec-observability-review-20260926.md)。操作流程见
[`tools/xrgame/README.md`](../../tools/xrgame/README.md#device-debugging-private-validation)。
本轮没有提交、推送或发布。构建在 Linux `/data00/xrgame-native`，设备为 AYANEO
Pocket DS / Android 13 / 4 KiB，系统 `TKQ1.230811.002`、
`eng.scm.20251104.212308`。序列号、完整 fingerprint 和 boot ID 保存在私有证据中。

## 输入

- APK SHA-256：`51a1251b06b5e641c7004274ae7465d770d1c936fee6ef18021c72ff8f807aa9`。
- 六组件 catalog SHA-256：`9c736d51bab86fed9d6305655a3a4e55ddcbc1ec32cad819f6e4d4c5f06ec83c`。
- Build IDs：`libwinlator.so` = `e120a35c35b69960ed4fe4c8993045bddffe4c1b`；
  `libgndownload.so` = `45a2263934179eae4cd7bc536e977519eeab61ae`；
  `libpulseaudio.so` = `ad97886e4ccdf2f02bec63d21c9733da0bfefe45`。
- Wine 源码：`tencentmalos/proton-wine` @ `5d0d333e7beef02fbb455cc3163b4e8e25615458`。
  本地 `feature/malos/wine-guest-debug` 的改动同步保存为父仓 patch，未推进 gitlink。
- 最终 x64 WineDbg SHA-256：`62eaafb1f3397af3fe5b62268147e91302b674e683bb541444a3d20a230ac4d0`。
  调试器仅放在私有证据和设备测试 prefix 中，未列入生产组件。

## Native

MCP session `54c2961d9f90495d8a7df259dd97c8c2`，app PID `24731`，
start ticks `1549687`。10:47:57.290Z–10:49:31.376Z，约 94 秒会话窗口，
包含暂停，不能作为游戏稳定运行时长。AGDE LLDB 21 完整工具链通过 attach、
pause、事件等待、寄存器/线程栈检查、continue、detach。
主线程栈包含 `__epoll_pwait → android::Looper::pollInner → pollOnce`。
清理后同一 PID 仍存活，`TracerPid: 0`，无残留 owned server/forward。

失败尝试也保留：默认工具发现没有选中可用 LLDB；混用 CodeLLDB 22 与另一套
LLDB 库导致宿主 SIGBUS。不能把这些失败算作设备崩溃。当前 package 绑定 API
拒绝 `A:\Ship\Hades2.exe` 这种 Wine 子进程名，因此 native 验证范围仅为 app。

## Guest

最终 MCP session `gdbg_a0dcdf12cfce4b9384fca0b5b9bf6c18`，Hades II
Android PID `9963`，start ticks `1856199`，Wine PID `0x012c`。
11:22:35.004Z–11:23:14.308Z，约 39 秒调试会话，包含两次暂停。
新启动的这代进程在 19:18:36（设备时间）完成 Renderer Initialized，19:18:39
创建三张 swapchain image；截图仍是黑屏和鼠标指针。

通过 x64 / little-endian 握手、线程/模块枚举、主线程和渲染相关线程寄存器、
栈内存读取，以及 pause → continue → pause → detach。前一轮同代进程枚举了
29 个 Wine 线程、14 个 x64 模块；ARM64EC 模块不能按这份 x64 列表视为完整枚举。
读取的是 Wine 提供的 Windows x64/ARM64EC context，原始栈内存不是已展开的调用栈。
FEX 自定义 mixed-stack、单步、软件/硬件断点尚未验证。

修复及保留的失败证据：

1. 内置 ARM64 WineDbg 报告 `aarch64`，不满足 x64 guest 连接要求；新增固定源码的 x64 构建。
2. Wine 的 Z 盘只映射 imagefs；直接传应用私有 Unix 路径会触发路径解析失败。
   helper 改为测试 prefix 中明确的 `C:\xrgame-debugger` 路径。
3. 已停止的 WineDbg 忽略 RSP interrupt；补入停止状态回复，使 MCP 得到真实 stop epoch。
4. FEX 缓存分配依赖首次访问异常；中止该异常会反复停在
   `FEXCore::LookupCache::FindBlock`（本轮 DLL RVA `0x169e4`）。可选透传首次 AV，
   保留 second-chance 停止；FEX 自用暂停 trap 仅按实际 `0xd4395fc0` 指令透传。
5. 旧 break-in 把调试器 ntdll 的地址传入目标，两个进程的 ntdll 地址不同，产生 AV。
   调试器现在从目标 PE 导出表解析 `DbgUiRemoteBreakin`。
6. r6 的 MCP 返回 cleaned 后，WineDbg 仍留在运行状态的事件等待循环。证据保留，
   在确认 kill-on-exit 已禁用后仅结束该 owned sidecar；r7 补齐这条 EOF 分支，
   helper 已实际正常退出。清理同时核对进程、forward 和游戏 generation。

关键证据 SHA-256：native evidence manifest
`88e5ee05c386192db09c4973f700dee8e49debccb99a5eadcb69b9a8c40b7cb8`；
guest r7 results `9b2334749026d22184b41733dd1a76ad1d6bf43849881fc55666e573cddd91ed`；
guest cleanup proof `1155939566b9e3f2dc3c6b97ed747d0fde3a2a043efb7d7e46e7b8d99f475c6b`。
最终 patch SHA `deb99807917e2cb644d845fb17f916eafb6d6cf329b302d3d534c500b0c32afc`。

私有证据位于 `wp3/20260926/hades2/debugger/`：native success/failed 导出，
各版 WineDbg、构建 JSON、proxy 日志、`guest-r*-results.json`、cleanup proofs。
失败和中断记录保留，不提交游戏内容或账号信息。

## 仓库维护

13 个直接子模块的 URL 均已指向 `tencentmalos`，gitlink 不变；其中 GBE 和 Turnip
继续以独立子仓维护。新增运行时与工具链源码 fork 包括 ntsync、DXVK、VKD3D、
PulseAudio、termux-on-gha、llvm-mingw；详细范围见 `references/README.md`。
Linux 既有源码 checkout 的 origin 也已切换，原地址保留为 upstream。
嵌套第三方依赖仍固定上游 pin；工具链 Release 归档仍按既有 SHA 使用上游资产。

Turnip 本地 `malos/main` 和迭代分支来自 shadPS4 的
`codex/turnip-xr-fdm2` @ `d15b7c019c8daa17e80051258077d9b2d5146a2b`。
新分支尚未发布，`.gitmodules` 仍引用已发布分支。
