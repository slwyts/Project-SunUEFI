# 临时启动

用 `fastboot boot` 把 UEFI 载入内存试运行，重启后平板恢复原来的启动方式，什么都不会写入。日常安装请用[引导助手](install-from-artifact.md)。

临时加载不会把 Linux 装进平板。要进入 Linux，平板里需要已经有 `sunuefi_esp` 和 `sunuefi_root`。

## 准备

* 小米平板 8 Pro，Bootloader 已解锁，重要数据已备份。
* 电脑装有 `adb` 和 `fastboot`，并有一根 USB-C 数据线。
* `SunUEFI-UEFI-Fastboot.img`：下载 Build products 里的 `SunUEFI-UEFI-Fastboot` 并解压。也可以自己构建，输出在 `artifacts/product/`，见[构建手册](../devel/building.md)。

这个镜像只含 UEFI，不含 Android 内核。助手安装的合体 BOOT 则把同一个 UEFI 和设备当前的 Android 内核打包在一起。

## 1. 进入原厂 Fastboot

在 Android 里打开 USB 调试，连接电脑：

```sh
adb reboot bootloader
fastboot devices
fastboot getvar unlocked      # 应显示 unlocked: yes
```

## 2. 临时加载

```sh
fastboot boot SunUEFI-UEFI-Fastboot.img
```

返回 `OKAY` 只说明原厂 Bootloader 收下了镜像。接着确认 UEFI 在运行：

```sh
fastboot -s SunUEFI-piano getvar product     # 应返回 piano-sunuefi
```

查询没有返回时，UEFI 仍可能在运行，只是 USB 还没连上，可以多试几次。

## 3. 启动 Linux 或回到 Android

平板里已经装有 Linux 时，请求从 ESP 启动：

```sh
fastboot -s SunUEFI-piano oem boot-stable
```

UEFI 会从启动镜像（ESP）直接启动 Linux。不想继续时，重启：

```sh
fastboot -s SunUEFI-piano reboot
```

## 遇到问题

* 屏幕白屏、没有响应：先运行 `getvar product`，再看[已知问题](known-issues.md#uefi-菜单与-usb)。
* 卡住，或电脑看不到设备：见[故障恢复](recovery.md)。
* 需要日志和截图：见 [UEFI Fastboot](fastboot.md)。
