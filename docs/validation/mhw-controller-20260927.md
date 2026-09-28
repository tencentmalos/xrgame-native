# MHW 手柄桥接修复（2026-09-27）

状态：SDL 加载失败已修复并保留数据更新至 AYN；同一 Wine prefix 的 XInput
探针已读到手柄、A 键按下/松开和物理左摇杆变化。游戏已进入冰原场景。
完整按键、右摇杆、扳机和震动未验收；没有接入 Foundation 音频或手柄模块。

## 根因与范围

Android 已识别名为 `Odin Controller` 的内置手柄，分配到第一个玩家，
共享内存的 connected 标记为 1。容器启用了 SDL controller API，输入类型为 BOTH。
但 evshim 只尝试 `dlopen("libSDL2-2.0.so.0")`，实际 bionic imagefs 和
Wine winebus 使用的是 `libSDL2-2.0.so`。修复前日志明确记录 versioned SONAME
不存在，虚拟 Xbox 手柄未能创建，因此游戏只得到键鼠输入。

`app/src/main/cpp/evshim/evshim.c` 保留原名称，并在失败时尝试 bionic 的名称。
这是共用 Wine 手柄路径的问题，不是 MHW 专属键位配置；同一运行时的其他游戏
也可能受影响，但本轮只实测 MHW。没有改游戏文件或配置、存档及手柄映射。

## 设备与构建身份

- AYN Thor，Android 13 / API 33，4 KiB pages；app targetSdk 36。
- Build：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
- Boot ID：`8bb14501-5800-4b9b-a9ab-7173603812f7`。
- APK SHA-256：`c3c220cd9d4999b031324055a54c63dbdd71fef2801a941be3a83bfae3250055`。
- Component catalog SHA-256：`f4f34d00c5e207b65b583f4be1bedcd85a4ffffac6bd7e6c36a9443339c2159f`。
- Build IDs：
  - `libevshim.so`：`5d357cd7a9756016f8b6eb3d1bfc9cc23edb262d`。
  - `libwinlator.so`：`e120a35c35b69960ed4fe4c8993045bddffe4c1b`。
  - `libvulkan_renderer.so`：`36421bccc5e74ef19b0ff30b47b21ea071507eab`。
  - `libgndownload.so`：`674e470eca24f48e67e09f5329fc7e7a58b98469`。
  - `libxrgame_debugbus.so`：`7182d65276c0f44d49b55fa761b4ca5ceef1b19a`。
- `assemblePicoXrDebug` 成功；APK 审计 123 项、0 错误；未提交或推送。

## 实测与边界

- 修复前 Android PID `13459` / MHW `14904`：17:44:12 日志多次出现
  `dlopen SDL failed: library "libSDL2-2.0.so.0" not found`。
- 修复后 Android PID `18530` / Winebus `20333` / 初始 MHW `20443`：
  17:53:39 起记录 `SDL 2.32.10 bound` 和 `P0 virtual joystick ... connected`。
  首轮设备采集墙钟 57.24 秒；之后观察到 MHW PID `23903`，不将两个 PID 混为
  同一游戏进程的连续验证。
- 新增只读 `tools/xrgame/fixtures/xinput-probe.c`。使用当前运行会话的
  `HODLL`、canonical WINEPREFIX、NTSYNC 和库路径启动，不接管游戏或注入输入。
  第一轮 3 秒探针共 30 次采样，slot 0 返回 `ERROR_SUCCESS`，其他三个槽返回
  `ERROR_DEVICE_NOT_CONNECTED`；A 键 `0x1000` 按下/松开使 packet 从 0 到 2。
- 第二轮探针 Android PID `25603` / Wine PID `1260`，12 秒 guest 计时内取得
  119 次 slot 0 成功采样，packet 最终为 706；左摇杆 X 为 -32767…32767，
  Y 为 -32768…32766，另观察到右肩键 `0x0200`。这是外部物理操作产生的输入，
  本轮没有用 ADB keyevent 或写共享内存伪造输入。
- 第二轮 host runner 在 37 秒墙钟超时，但 guest 后续自行完成，日志最终到
  11939 ms，进程表确认探针已退出。保留超时记录，不把 guest 计时当 host 时长。
  更早缺少 HODLL 的启动失败也保留，不能作为修复前 XInput 测试结果。

原始日志、截图、APK 审计和失败探针位于仓库外
`xrgame-native-evidence/mhw/20260927/input/`；账号、游戏数据和二进制不入库。
