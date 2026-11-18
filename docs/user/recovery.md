# 返回 Android 与故障恢复

SunUEFI 不替换原厂引导程序。正常扫描和启动不会改写 Android 分区；手动执行 [UEFI Fastboot 刷写](fastboot.md#刷写分区)时，只写指定的目标。出问题时，按下表处理。

| 现象 | 原因 | 怎么做 |
| --- | --- | --- |
| 在 Linux 里想回 Android | 保存的启动路线是 Linux | 运行 `piano-next-boot android --reboot`，或点桌面上的“返回 Android” |
| 在 Android 里想改回 Linux 或 Android | 路线保存在 BOOT 分区的记录区 | 在助手的 **启动** 页重新选择；没有助手时见下文“用命令改启动路线” |
| 屏幕停在原厂 Fastboot | 没有选到系统，或刷写后没有重启 | 电脑上运行 `fastboot reboot` |
| 电脑能看到 `SunUEFI-piano` | 固件还在运行 | 运行 `fastboot -s SunUEFI-piano reboot`，固件保存设置、同步存储后重启。看到开机选择页时，可用音量键选择 Android |
| 完全没有反应 | 死机或卡在早期启动 | 长按电源键强制重启，直到出现开机画面。按多久因机器而异，用你已知的原厂强制重启方式 |
| 想进小米原厂 Recovery | 需要原厂恢复功能 | 在 Android 里运行 `reboot recovery`，或用系统的重启菜单。不会触发 SunUEFI，也不改变保存的启动路线 |

强制重启后仍然回不到 Android 时，先不要继续刷写，记下屏幕状态和电脑上 `fastboot devices` 的输出再求助。

## 更新报启动分区空间不足

运行 `df -h /boot/efi` 检查空间，用 `uname -r` 和 `bootctl list` 区分当前内核与旧版本。
保留当前版和至少一套已能启动的回退版本，只卸载用不到的旧内核及其同版本配套包。
用系统包管理器卸载，让它同时移除相应的启动文件，不要手动清空启动分区。
如果更新已经因空间不足中断，腾出空间后先重新生成报错版本的启动文件，
确认内核、initramfs 和设备树齐全再重启，见[构建手册](../devel/building.md#从装包到启动项)。

## 用命令改启动路线

在 Android 里用 root 执行。BOOT 分区按当前槽位选择：

```sh
BOOT=/dev/block/by-name/boot$(getprop ro.boot.slot_suffix)
piano-boot-request status --device "$BOOT"
piano-boot-request set --device "$BOOT" --target android
```

`--target` 可选 `android`、`menu`、`linux`、`setup`。BOOT 分区里没有有效记录时，系统默认进入 Android。

## 原厂 Recovery

SunUEFI 不使用 Recovery 分区安装。原厂引导程序不会为独立的 Recovery 镜像加载 vendor_boot 和 pvmfw，启动会停在 Fastboot，所以安装器拒绝 `--recovery`。如果之前自己刷过类似的镜像，请恢复原厂 Recovery 镜像。保存了 Linux 路线的同时进入 Recovery，在设备上还没有试过。

## 关于数据

`fastboot boot` 只在内存里运行。安装器和助手只写 `sunuefi_esp` 和 `sunuefi_root` 两个分区，不碰其他分区。合体 BOOT 会改写 BOOT 分区，恢复原样需要原厂 BOOT 镜像，可以用[合体 BOOT 的还原工具](../devel/boot/combined-boot.md)无损还原。动手之前先备份 BOOT 分区。
