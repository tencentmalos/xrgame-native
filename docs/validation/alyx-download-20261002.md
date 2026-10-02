# Half-Life: Alyx 空文件导致续传失败（AYN，2026-10-02）

状态：**已修复，AYN 保留原下载内容续传完成，UI 显示“已安装 / 开始游戏”**。
范围是 Steam 安装（app 546560），不代表 Alyx / OpenVR / XR 运行验收。
原始日志、缓存 manifest、截图、APK 和审计结果保存在仓库外
`xrgame-native-evidence/alyx/20261002/download/`。

## 根因与修复

旧包在四次续传中均通过 CDN / depot 准备，随后在 depot 546561 收尾时失败：

```text
write_depot: final open '.../Half-Life Alyx/game/hlvr/cfg/deckard/dummy.json':
No such file or directory (os error 2)
```

缓存 manifest `546561_1436897171613240821.manifest` 包含 2771 条记录。
以既有 manifest 解析工具匹配该文件的路径哈希后，确认它是合法的 **0 字节、0 chunk
普通文件**；设备上其父目录 `deckard` 不存在。不能把空文件当作损坏内容而跳过。
设备当时仍有约 315 GiB 可用空间。

`DepotFiles::acquire()` 原本会在写 chunk 前创建父目录；空文件不产生 chunk job，
因而只能进入 `finalize_remaining()` 的直接建文件分支。该分支漏掉了父目录创建，
在游戏主体文件完成校验后抛出 ENOENT，安装状态始终无法提交。

在该分支补齐 `make_parent_dirs()` 和与普通文件一致的 `set_file_mode()`，保留已有
定长截断、同步和路径校验行为。没有改写 manifest、删除安装目录或跳过空文件。
此修复覆盖所有使用同一 Steam 下载器的游戏，不是 Alyx 专用路径特判。

## 本地验证

- 新增 `writer_finalizes_nested_empty_files_without_directory_entries`：清单不包含目录
  记录；覆盖只有空文件、已有有效数据加空文件、单 worker / 并行验证路径；检查新建
  嵌套空文件、旧文件截断为零、文件模式和已有数据不变。测试不访问外部 CDN。
- 先加测试再修生产代码，旧逻辑复现相同 ENOENT；修复后通过。
- `cargo test --locked`：215 通过、2 失败。两个失败是 `store_dl::tests` 原有的大小写
  拼写断言；在 macOS 不区分大小写的文件系统上，精确查找快捷路径保留请求拼写。
  用临时源码副本恢复 HEAD 的 `depot_writer.rs`，这两个失败同样复现（3 通过、2 失败）。
  本次没有扩大修改范围去调整大小写行为或这些既有测试。
- `:app:assemblePicoXrDebug` 成功；删除旧输出 APK 后重新打包，审计 123 条目、零错误。
  新旧 APK 的 ZIP 条目对比仅 `lib/arm64-v8a/libgndownload.so` 内容发生变化。

## 设备与构建身份

- 设备：AYN Thor / Android 13 / API 33 / 4 KiB，targetSdk 36。
- fingerprint：`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
- boot ID：`8bb14501-5800-4b9b-a9ab-7173603812f7`。
- 旧包 SHA-256：`053ed5ea044a33b185db1ec0761bee79d09a04e5876cb31e0c7d57c71be3a471`。
- 修复包 SHA-256：`a561b364b6a7645539c8a0391a74e931132d36388a98c8db391f30d5664113bd`；
  安装后读取设备 base.apk 的 SHA-256 再次匹配。
- catalog SHA-256：`e8619cda78b93fbde4e3f509157476f08db3ecb04749215e1fa4eb885984f353`。
- `libgndownload.so` Build ID：`d2351fd2a3bfd2f5ec3f4074e921d6d6f4f46a9e`。
  全部 ELF Build ID / SHA-256 见私有 `apk-audit.json`。
- 旧进程 PID 5208；更新并重新打开后 PID 17428，完整 generation 见
  `device-replay-start.json`。

## 设备复测

旧进程四次失败记录：14:51:44.529–14:53:11.499、14:53:32.274–14:54:55.283、
14:55:07.115–14:56:30.623、14:57:04.854–14:58:27.521（设备本地时间）。
保留这些失败日志，不将其计入修复包结果。

保留数据更新后，15:08:49 设备系统窗口服务发生 Watchdog 超时，Android framework
重新启动，boot ID 未变。首次启动 / 输入命令因此返回 Broken pipe，尚未进入续传测试；
失败命令及 `logcat-system-restart.txt` / `crash-after-update.txt` 已保留。恢复后重新打开
应用，账号会话、库和原下载记录仍在。不能把这次系统重启归因于尚未运行的下载器修复。

15:11:21.256 在新 PID 中从 UI 点击“继续下载”，恢复记录为 **72628685840 字节**；
15:11:22.239 进入 native 下载器，随后校验已有文件。

| 阶段 | 实测结果 |
| --- | --- |
| 主 depot 546561 | 约 83 秒验证已有文件，网络传输计数为 0；空文件正常创建，进入下一 depot |
| depot 546563 | 下载完成，随后进入 1295040 |
| depot 1295040 | 下载完成，native 回调成功 |
| 完成回调 | 15:14:46.962：76148292967 字节，3 depot 完成、3 已安装的公共依赖 depot 跳过 |
| 安装状态提交 | 15:14:46.970，随后 UI 显示“已安装 / 开始游戏”，大小 71.06 GiB |

从本轮下载注册到安装状态提交为 **205.714 秒**。回调字节数包含复用文件，不能当作本轮
网络下载量。`dummy.json` 实测存在且为 0 字节，父目录由修复后的下载器创建；没有手工
补目录掩盖失败。结束 PID 仍为 17428，boot ID 不变。

结果见 `replay-summary.log`、`device-replay-complete.json` 和 `screen-completed.png`。
本轮未执行额外的独立 DepotDownloader / 全文件哈希对照，也未启动游戏；不扩展为
原始 WP1 全项验收或 Alyx 运行兼容性结论。
