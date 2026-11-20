# 桌面电源管理与睡眠

桌面和 logind 负责按键、锁屏与空闲计时。使用电池时可挂起到内存；
外接电源时保留熄屏，系统继续运行充电保护。

休眠与睡眠分别处理。休眠需要可恢复的磁盘交换空间、内核恢复入口
及固件启动路径；当前的 zram 在断电后不会保存内容，不能作为休眠存储。
仅有内核 `CONFIG_HIBERNATION=y` 不代表系统已经可以休眠。

## 睡眠与唤醒

默认使用 `mem/deep`，启用普通挂起，屏蔽磁盘休眠、混合睡眠及先挂起后休眠。
RTC 闹钟和电源键唤醒已有实机结果，显示、USB 网络、音频和 DSP 可以恢复；
长时间挂起、待机耗电及全部外设组合仍需继续检查。
默认空闲时间、锁屏和按键策略见[电源键、合盖与睡眠](power-button.md)。

板级设备树用 SM8750 主线的 `qcom,rpmh-stats` 描述固件的睡眠计数区。
Linux 的 `qcom_stats` 从 `/sys/kernel/debug/qcom_stats/` 提供 `aosd`、
`cxsd`、`ddr` 和各子系统的进入次数与累计驻留时间。平板冷启动已读到
计数和 DDR 频点记录；这些统计需要在拔线前后对照，单次读数不代表
进入深睡或达到原厂待机时间。

## 插电熄屏

HyperOS `OS4.0.6.0.XPYCNXM` 的充电驱动在线时持有 `charger_wakelock`，
断线或暂停 USB 输入时释放。屏幕关闭并不等于整机挂起。

Linux 的充电驱动在接线或无法确认已经拔线时拒绝挂起，以便继续监测两路
USB 接口温度并在异常时停止充电。GNOME 和 Plasma 默认插电只做空闲
熄屏，不再自动请求挂起。桌面未接管合盖时，logind 在外接电源上锁屏，
仍保留电池供电时的合盖动作。

## RTC 与原厂时间

PMK8550 的 RTC 是持续运行的秒计数器，Android 用独立的持久化偏移得到 UTC。
同机原厂 `time_daemon`、`libtime_genoff.so` 和文件读回确认：
`persist/time/ats_2` 是八字节小端毫秒偏移，除以 1000 后与 PMIC 秒计数相加，
得到 Android 的 UTC。毫秒小数在秒精度的 RTC 接口中舍去，不调整原始计数。

`uefi/components/rtc/` 仅以只读模式打开该文件，检查精确长度和数值范围，
将秒偏移写入独立分配的 `EfiReservedMemoryType` 页。Linux 设备树副本为该页
创建 `nvmem-rmem` 提供者、`fixed-layout` 及四字节 `offset` 单元，通过已有的
`nvmem-cells`／`nvmem-cell-names` 连接到唯一匹配的 PMIC RTC，不要求 RTC
节点已有 `phandle`。普通 EFI 与 BOOTv2 使用同一函数；输入设备树和 Android
文件保持不变。

该页有 `no-map` 和 `read-only`，退出 UEFI 后仍保留，内核内存检查按保留页
验证，不放宽对 DMA 或其他内存的检查。`rtc-pm8xxx` 使用现有 NVMEM 后端读取
偏移，标准 `rtc0` 提供日历、相对闹钟和挂起计时，不需要 rootfs 专用校时服务。
固件文件缺失、无效或无法分配页时跳过交接；保护的计数器缺少有效偏移时跳过
HCTOSYS，保留相对闹钟，不把 1970 年计数当作系统日期。

Piano 内核启用 `RTC_HCTOSYS`，仍关闭 `RTC_SYSTOHC`；NVMEM 提供者没有写回
回调。Linux 不改 PMIC 原始日期，也不把时间写回 Android 的 `ats_2`。
如果 Android 尚未保存偏移，联网校时仍可设置 Linux 系统时间。

Linux 7.3.3 实机已在启动时从 `rtc0` 设置正确日期，`hctosys=1`；
日历时间与系统时间相差不超过一秒。带偏移的 `+22` 秒闹钟完成 `deep` 唤醒，
启动 ID 不变，挂起成功计数加一、失败为零，恢复后两种时间继续一致。
