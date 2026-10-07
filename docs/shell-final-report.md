# Shell 最终 RAM 摘要

第 67 次 `private/analysis/ramlog-test-67/uefi.txt` 最终报告确认 7 个真实 UFS SFS 卷、全部 readonly=1。LUN0 卷有 4096 字节普通文件读取证据、CRC32=7C5D9A17；其他 6 卷来自 parent_index=4。该捕获没有保留 Shell 命令结果，因此不能补推 Shell 已成功运行三个命令。

现新增的摘要仍在 RamApp 的静态 RAM 数据内，不是磁盘文件或 UEFI 持久变量，不会跨重启保存。每次自动启动的三个命令有独立记录：`map-r`、`dh-simplefilesystem`、`drivers`。保存实际 LoadImage、LoadedImage protocol、StartImage、显式 UnloadImage 状态，以及 requested/start_called/start_returned/unload_called。

即时日志覆盖时，`PianoUfsReadOnlyDma.c:HaltService()` 在紧凑 UFS/FAT 报告后调用 `PianoReportShellDiagnostics()` 重新输出。只在准备脚本选 `--ufs-shell` 时加入 `PIANO_UFS_SHELL`，纯 BlockIO/Filesystem profile 无未定义的 Shell 依赖。正常停止、恢复 timer 和 EBS halt 都沿用该报告路径，没有增加分配、free 或协议 teardown。

```text
SUNUEFI_SHELL_REPORT_BEGIN initialized=1 planned_commands=3
SUNUEFI_SHELL_REPORT command=map-r requested=1 execution=returned-success ...
SUNUEFI_SHELL_REPORT command=dh-simplefilesystem requested=1 execution=returned-success ...
SUNUEFI_SHELL_REPORT command=drivers requested=1 execution=returned-success ...
```

这是格式示例，尚未获得实机新摘要。实际 `execution` 也可能为 `not-requested`、`not-started`、`in-progress` 或 `returned-error`。若 timer 在 FAT 检查或调用 Shell 前触发，明确输出 `initialized=0` / `commands_requested=0`；不会用零初始化的 EFI_SUCCESS 填补未执行步骤。若 Shell 尚未返回，start_called=1、start_returned=0、start=Not Started，与完成成功不同。

标准 UEFI application 正常 return/Exit 通常已由核心卸载。因此保留既有显式 UnloadImage 调用时可能得到 Invalid Parameter；摘要同时记录 `auto_unload_expected=1`，该 unload 状态不应直接被解释为 Shell 命令失败。判断 image 执行结果使用独立 start 字段。摘要说明 image 是否启动/返回及其状态；命令输出内容仍由 `SUNUEFI_SHELL_OUTPUT` 保存，不能用摘要伪造映射名或文件内容。

`bash tests/native/test_ufs_firmware.sh` 已通过 loader 状态与最终 re-report 的 ASan/UBSan 检查、正常/EBS halt hook 检查和 AARCH64 语法检查。该补丁未执行 prepare/fullbuild/device，未写入第 67 次已验证镜像。
