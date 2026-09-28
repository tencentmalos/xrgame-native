# 个人版 spec 实施状态（2026-09-27 续验）

当前按 [个人版 v2](../specs/xrgame-native-personal-v2.md) 推进。主干仍为
`malos/main`；以下记录功能与 AYN 实测状态。源码已纳入
[2026-09-28 阶段提交](stage-20260928.md)，不代表 APK 或运行时公开发行。
**P1–P3 已有主要功能闭环，但未把全部原始 spec 标为完成。**

| 项目 | 已落地 / 已验证 | 仍需完成 |
|---|---|---|
| P1 执行与游戏基线 | app 域 exec/mmap 行为矩阵；x64 控制台、GDI、D3D11；Hades II 场景、键盘输入、本地存档恢复、覆盖更新保留；已有十分钟与三次启动退出记录 | 物理手柄与扬声器实听；偶发启动失败仍保留未闭合状态 |
| P2 Steam 安装 | Balatro 完整下载、损坏修复；AI Limit 强制中断续传、3050/3050 整文件哈希；无安装 journal 的目录重新识别；公共运行库与独立 DepotDownloader 5/5 对照 | 完整付费游戏的 DepotDownloader 对照需交互登录；独立 PC Steam 来源导入；完整运行流量审计。AI Limit 尚未运行验收 |
| P3 显示 | AHB GPU 拷贝与直接采样均能显示；resize、隐藏/恢复、退出；一次同场景比较；修复 DXVK 延迟清屏导致的黑屏，无新增 CPU 读回 | 未证明稳定性能收益，默认保留 GPU 拷贝；非零 wait fence / target MSC 尚缺用例 |
| D0/D1 调试身份与分析 | PID/启动世代拒绝旧请求、私有 evidence/journal、PE 身份与 guest 寄存器、app native 和 guest 基础调试、失败清理记录 | Wine 特殊进程名的 native provider 接入；完整可信 mixed unwind；ASLR/异常负例矩阵 |
| D2 执行控制 | 修复 FEX 跨进程代码失效死锁和 RET 缓存绕过单步；自有 fixture 的未来断点、移除、继续、逐指令 CALL/RET、旧 stop epoch 拒绝与正常退出 | 任意寄存器写入、并发线程控制、观察点；软件断点继续 opt-in |
| D3/D4 性能分析 | Present/AHB/host 提交与释放关联、生命周期 trace；Foundation DebugBus host 状态/进程/模块/Present 查询实测；未假装缺失的 fence 时间为 GPU 执行时间 | 有实际热点后再接 ARM64EC 函数探针；不将 shadPS4 前端能力算成本项目已实现 |

Swan/XR、Linux 构建依赖迁移继续按用户要求延后；原 WP3 的远端 CI、发行来源闭包与
公开发布仍在最后的可选 P5。它们没有被当前 AYN 结果替代。

证据入口：

- [P1 与游戏基线](personal-p1-p2-ayn-20260926.md)
- [Steam 下载、续传、校验与导入](personal-p2-downloads-20260927.md)
- [显示、调试根因与修复验收](personal-p3-debuggers-20260927.md)
- [与原 spec / shadPS4 的逐项对照](spec-observability-review-20260926.md)
- [MHW：三项启动阻塞已修复；AYN 到达标题画面、开场剧情和角色创建](mhw-startup-20260927.md)
- [MHW：原云存档 4/4 恢复并校验；已购 DLC 接通、Iceborne 菜单通过](mhw-saves-20260927.md)
- [MHW：双向 DX11/DX12 自重启、原 3 个角色识别及正常退出通过；调试轮中断保留待查](mhw-restart-20260927.md)
- [MHW：修复 SDL 名称不匹配导致的手柄失效；XInput 读到物理按键和摇杆](mhw-controller-20260927.md)
- [MHW：CPU 调度、guest 热点与 AHB fence 等待的当前场景采样](mhw-performance-20260927.md)
- [MHW：Wine / ntsync 同步次数及分段每 Present 统计](mhw-sync-20260927.md)
- [MHR：修复媒体依赖缺失黑屏，已读原存档进入曙光村庄；包内升级重放待验](mhr-startup-20260927.md)
- [默认随包组件、损坏自动修复与 D3D10/11 实测](bundled-runtime-20260927.md)
- [Foundation DebugBus：host 查询与 Present 日志](debugbus-20260927.md)

当前默认源码配方纳入 DXVK xrg2 与 FEX xrg5。完整 APK/catalog/库身份、设备世代、
PID、时长和失败记录见上述对应记录；原始截图、日志、游戏数据和 APK 均在仓库外。
