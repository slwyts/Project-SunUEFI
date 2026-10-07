# 已知问题

## UEFI 白屏

部分集成版本在 Logo 后白屏，显存截图却能看到菜单。时钟初始化是调查方向，但保住显示 AHB 后仍出现白屏，确切原因尚未确认。Linux 后续接管显示后已有正常进入桌面的记录。

白屏时先查询 `fastboot -s SunUEFI-piano getvar product`。连接正常且设备已部署 Linux 时，可请求 `oem boot-stable`；没有连接则参考[返回 Android](recovery.md)。最新显示兼容候选尚未实机验证。

## Linux 启动较慢

最近一次从真实 ext4 根分区启动到图形目标约 130 秒，其中 Linux 报告 58.748 秒 kernel、71.725 秒 userspace，不包含 UEFI 时间。存储出现、显示初始化和服务等待均需要继续分析，暂时没有确定的提速结论。

## Recovery 持久启动

独立 Recovery 加载路径缺少依赖，当前保留原厂 Recovery。合体 BOOT 的前置选择器已正常启动 Android，上一版也已验证标准 `reboot recovery` 进入 Mi Recovery 并返回系统；显式 UEFI 请求及自动消费尚未完成。详见[入口与请求](../devel/reboot-request.md)。

## 麦克风与桌面性能

Linux 扬声器左右测试正常；DMIC1 能录到可辨认声音，但噪音较大，GNOME 输入音量条仍异常。声音路径和录音质量还在调试。

窗口拖动/缩放曾有约 30 次每秒的显示提交记录，其他桌面手势更流畅。它不等于全系统只有 30 fps，原因仍待定位。

## 关机与其他硬件

Linux 关机曾重新进入 Android。日志确认用户态执行了关机流程，最终断电或重新唤醒的原因尚未定位。双电池、基础 PD 充电与 UPower 状态已有实测；高压快充、合盖、休眠恢复及完整充电流程仍未验收。

蓝牙目前确认到发现设备；UEFI 的触屏、官方键盘/触控板、USB Host 和持久变量尚未完成。完整范围见[项目状态](../status.md)。
