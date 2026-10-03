# AI Limit 启动检查（AYN，2026-10-02）

本轮以设备现有 build22、原运行配置进入语言设置及“新游戏 / 设置 / 结束游戏”主菜单。
没有修改代码、替换 APK 或游戏文件，也没有清理 shader cache。这是一次启动观察，
不代表此前退出原因已修复、实际关卡或重复启动已验收。

## 两个不同现象

- 用户此前启动：app PID 15588、guest PID 16840。Unity 2022.3.39f1 已初始化
  D3D11；最后一条业务日志是 `VersionValidationManager` 写入 `VersionLocal` 时间戳，
  然后打印正常关闭时的内存统计。loader 等待 20,069 ms 后记录 process exit=0。
  这不足以认定 native crash，也不能仅凭日志顺序断定版本初始化主动要求重启。
  退出触发点尚未取得调用栈证明。
- 受控重放：同一 app PID，guest PID 20816 / start ticks 172116840。
  黑屏时 `Player.log` 明确记录“预编译变体”；TID 20934、20935 的
  `dxvk-shader-l` 编译线程在保留的 top 样本中分别为 96.2%、92.5%
  （每线程 100% 表示一个 CPU 核心）。随后出现语言设置，最终出现完整主菜单。
  编译在部分 UI 已显示时仍继续，不能把全部加载时间都归因于编译。

观察到 Mesa shader cache 占用 43,740 KiB；本轮保留缓存。Unity 实际使用
**Direct3D 11.0 / feature level 11.1**，虽然容器 `dxwrapper` 配置字段为 `vkd3d`。
不能只凭 UI 配置字段把它当作 D3D12 问题。

## 身份与边界

- AYN Thor，Android 13 / API 33 / 4 KiB，targetSdk 36。
- APK SHA-256：`4607aeda2dd6dfb209c3e512a98d2f242ecc579b8afe4c975c9f5859049ed397`；
  从设备 base.apk 重读核对。
- 组件 catalog SHA：`dbf85e43cfb5e1f604507a5cb466cd4ffb71e57560a4830b33f02a6fadb8935f`。
- boot ID、fingerprint、全部 ELF Build ID、进程 generation、完整观察时长及私有证据
  哈希见[同名 JSON](ai-limit-startup-20261002.json)。观察时长包含启动及菜单停留，
  没有用于声称精确首帧时间或 FPS。
- 容器前后仅 `sessionMetadata` 改变，运行设置不变。仅注入一次 Android R1 和
  一次 Tab；不以这两次输入认定物理手柄、游戏内移动或关卡验收。
- 初始失败记录、Player/loader 日志、线程样本与截图保存在仓库外私有证据目录。
  未启动 debugger、未创建 ADB forward 或临时设备 payload；结束镜像采集后保留游戏现场。

下一步若再次出现退出，应在保留当前版本文件及缓存的前提下，捕获正常退出路径的
调用栈与可重复触发条件；不把零退出码改为无条件自动重启。
