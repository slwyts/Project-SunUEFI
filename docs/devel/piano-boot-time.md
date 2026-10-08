# Piano Linux 启动时间

2026-10-09 全新 Debian / GNOME root 的首次启动，内核
`7.2.9-piano-gnome-g60fd202096bf`，测得 **37.730 秒 kernel + 68.332 秒
userspace = 106.062 秒**。系统直接从 UFS ext4 启动，没有解压完整 root
归档。中文、Chromium 和默认动画均已在这次首次桌面中使用。

这次四组 SPMI 警告及堆栈的实际内核时间跨度合计约 9.18 秒，长堆栈
逐行输出到高分辨率 framebuffer 时，每行间隔约 35–55 毫秒。默认构建
已把控制台等级改为 `loglevel=4`：屏幕保留错误和更严重的信息，普通
警告与信息仍可从 dmesg / journal 读取。SPMI 失败本身并未因此修好，
这项变化的提速需要下一版启动对比确认；ramoops 的 console 镜像也会
受控制台等级影响。

首次传感器配置导入约 13.17 秒，后续启动已有配置时会跳过复制。
显示服务约 14.63 秒，并被通用系统初始化延后到内核时间约 81 秒开始；
还需要分别优化真实设备准备和服务依赖，不能把这些重叠时间直接相加。

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

完整SMMU/UFS路由及上下文转储默认关闭。需要定位DMA时，在内核命令行显式加入 `piano.boot-dma-log=1`，initramfs仍会在原来的加载阶段只读转储；这不是新的启动profile，也不改变DMA映射、模块加载失败处理或根分区选择。恢复a8的日志显示该转储发生在早期启动并逐行写入console/kmsg；下一份默认initramfs不再为正常启动执行这些诊断读取。尚未测量这项变化带来的耗时差，不把全部慢启动归因于它。

内核编译现改用固定的构建专用checkout，准备器仍保留每版不可变snapshot。此前每次更换带tree摘要的源码绝对路径，会改变所有对象的编译命令和依赖时间，即使只改少量文件也近乎整树重编。固定checkout在持有源/O/产物锁后按正常Git切换提交，manifest记录实际commit/tree及两份源码路径；runtime headers_install也使用同一compiler路径，不修改Kbuild的.cmd或依赖检查。现有已完成bde3 runtime身份仍可读取且完全一致；首次迁入新路径仍需整编，后续增量耗时尚未实测。
