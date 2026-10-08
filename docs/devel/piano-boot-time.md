# Piano Linux 启动时间

2026-10-08 同机正常启动，`7.2.9-piano-gnome-g45bba6e91e6c` 到
`7.2.9-piano-gnome-ga8c9650eb320` 加 CPUCP builtin 后，
`systemd-analyze` 总时间由 **127.098 秒降至 106.091 秒**。两次均使用真实
UFS ext4 根分区，initramfs 不解压 root tar。

| 实际时间 | 45b | a8 + CPUCP builtin |
| --- | ---: | ---: |
| kernel，包含 initramfs | 52.486 s | 45.667 s |
| userspace | 74.611 s | 60.423 s |
| 总时间 | 127.098 s | 106.091 s |
| `basic.target`，PID1 之后 | 48.395 s | 35.155 s |
| `piano-display.service` 自身 | 15.796 s | 14.802 s |

CPUCP 是 SM8750 SCMI mailbox 的真实供应驱动。默认构建现在合入
`linux/configs/piano-cpucp-early.config` 的 `CONFIG_QCOM_CPUCP_MBOX=y`，
使其在 `core_initcall` 注册；SCMI 协议、mailbox transport 和 cpufreq 也为
builtin。磁盘 initramfs 的模块依赖处理同时接受 module 与 builtin。

a8 的原始 journal 字段确认 SCMI bus 在 **1.405523 秒**注册，协议在
**15.649120 秒**成功通信，早于 PID1 的 45.667 秒。此前文本中的
54.500442/54.521800 秒是 journal 的 `__MONOTONIC_TIMESTAMP`，即 journald
接收这些 kmsg 的时间；真实内核时间保存在 `_SOURCE_BOOTTIME_TIMESTAMP`
及兼容的 `_SOURCE_MONOTONIC_TIMESTAMP`。不能据 `journalctl -o short-monotonic`
的接收时间推断 builtin 驱动仍在 PID1 之后才 ready。systemd v257 的
[kmsg 输入源码](https://github.com/systemd/systemd/blob/v257.9/src/journal/journald-kmsg.c#L236-L244)
保留两个 source 字段，
[显示源码](https://github.com/systemd/systemd/blob/v257.9/src/shared/logs-show.c#L452-L485)
因 BOOTTIME 与 MONOTONIC 的区别选择 journal entry 的时间。

这次总时间缩短 21.007 秒，同时包含 a8 的其它内核改动，不是单独切换 CPUCP
的对照。没有采样早期实际 CPU 频率，也没有完整的 45b 首阶段日志，不能把
全部耗时或改善归给一项。45b 显示服务没有消耗 150 秒 DSI 等待；
`dev-sda36.device` 的 28.823 秒是后续 udev/systemd 就绪时间，不能当作实际
UFS 根分区发现耗时。后续定位应使用原始 source 时间，并保留必需的驱动初始化。

私人原始记录位于 `private/provisioning/recovery-priority-20261008/` 的
`a8-cold-state.txt`、`a8-scmi-source-times.json`；45b 时间线及这次更正位于
`private/analysis/piano-boot-time-45-20261008/`。这些记录没有作为公开构建输入。
