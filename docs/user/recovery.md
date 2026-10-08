# 返回 Android 与故障恢复

SunUEFI 不替换原厂引导程序，也不修改 Android 的系统分区。UEFI 阶段对原厂关键分区由代码强制只读。出问题时，按下面的情形找到对应办法。

## 在 Linux 里正常运行

```sh
piano-next-boot android --reboot
```

它把“下次进入 Android”保存到合体 BOOT 的记录区并正常重启。不加 `--reboot` 只保存，不重启。桌面里也有“返回 Android”入口，会先弹出确认框。

## 在 Android 里想改回 Linux 或 Android

Android 里用 root 执行（设备路径是你的 BOOT 分区，不要预设 `boot_a`，先看 `status`）：

```sh
piano-boot-request status --device 分区路径
piano-boot-request set android --device 分区路径
```

可选目标：`android`、`menu`、`linux`、`setup`。没有有效记录时系统默认进入 Android。

## 屏幕停在原厂 Fastboot

```sh
fastboot reboot
```

## 电脑能看到 `SunUEFI-piano`

固件正在运行，只是屏幕可能白屏：

```sh
fastboot -s SunUEFI-piano reboot
```

固件会先释放 USB、存储、时钟和 SMMU 资源，再冷重启回 Android。

## 没有任何反应

长按电源键强制重启，直到看到开机画面，并确认进入 Android。具体需要按多久因机器而异，用你已知的原厂强制重启方式。如果还是回不去，先不要继续刷写，记下屏幕状态和电脑端 `fastboot devices` 的输出再求助。

## 小米原厂 Recovery

在 Android 里 `reboot recovery`，或使用系统的重启菜单，会进入小米原厂 Recovery，不会触发 SunUEFI，也不改变已保存的路线。从 Mi Recovery 里选择“重启 → 重启至系统”。

SunUEFI 目前不使用 Recovery 分区安装。独立的 Recovery 镜像会被原厂引导程序拒绝加载（缺少 vendor_boot 和 pvmfw），所以安装器不允许 `--recovery`。如果之前自己刷过类似镜像，请恢复原厂 Recovery 镜像。持久选择 Linux 的同时使用 `reboot recovery` 的组合，尚未在设备上验证。

## 关于数据

`fastboot boot` 只在内存里运行。安装器会写入 `sunuefi_esp` 和 `sunuefi_root` 两个分区，不会碰其他分区。合体 BOOT 会改写 BOOT 分区；恢复原样需要原厂 BOOT 镜像，可用[原生 BOOT 重打包工具](../devel/android-boot-repack.md)无损还原。动手之前备份 BOOT 分区。
