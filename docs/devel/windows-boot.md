# 启动 Windows 安装介质

这版固件带最小 ACPI 表集，覆盖处理器、GIC、UFS 和 xHCI，见 [ACPI 表](acpi.md)。
用它启动 ARM64 WinPE 或 Windows 安装介质，检查设备枚举。临时启动不改分区；
装到机器上会把同一份 UEFI 和设备当前的 Android 内核打包进 BOOT。

## 产物与签名

`./build.sh uefi` 输出 `artifacts/product/PianoUEFI-product.img`。这是纯 UEFI 镜像，
不刷写 BOOT，供 `fastboot boot` 使用。检查镜像里的五张表：

```sh
for s in 'RSD PTR ' FACP APIC GTDT DSDT; do
  printf '%-9s' "$s"
  grep -aob -- "$s" artifacts/product/PianoUEFI-product.img | head -n 1 || echo missing
done
```

`FACP` 后面应当能读到非零的 DSDT 地址；`AcpiTableDxe` 在安装 DSDT 时回填，
FACP 源文件里写的是 0。

## 临时启动

和[临时启动](../user/temporary-boot.md)相同，只是把下载的镜像换成刚构建的这一份：

```sh
adb reboot bootloader
fastboot boot artifacts/product/PianoUEFI-product.img
fastboot -s SunUEFI-piano getvar product     # 应返回 piano-sunuefi
```

`fastboot boot` 只在内存里运行，不写 BOOT、`sunuefi_esp` 或 `sunuefi_root`。
固件设置和 Windows 无关的文件仍会写进 `persist/sunuefi/`。

## 装到机器上

临时启动验证过 ACPI 和 USB 后再装。补丁、模块和合体 BOOT 的关系见
[合体 BOOT](boot/combined-boot.md)。

1. 用同一份源码构建模块：

   ```sh
   ./build.sh uefi
   ./build.sh module
   ```

   产物是 `artifacts/android/SunUEFI-Piano-版本.zip`。

2. 在 KernelSU 或 SukiSU 的模块页面安装这个 ZIP。提示出现时按音量＋，
   安装或更新引导，保留当前 Android 内核。
3. 正常重启。以后开机默认进入这个 UEFI；在开机选择页仍可临时选 Android。

这一步会改写 BOOT 分区，把 UEFI 和当前 Android 内核打包成合体 BOOT。
`sunuefi_esp`、`sunuefi_root` 和 Android 数据分区不动。更新前备份 BOOT，
还原方法和限制见[合体 BOOT](boot/combined-boot.md)。

不想改 BOOT 时只用临时启动。两者跑的是同一份 UEFI 核心。

## 准备 Windows 介质

固件的文件系统访问只覆盖 FAT 分区，Windows 安装 ISO 通常带 UDF，不能直接挂载。
把安装介质里的 EFI 文件复制到一个 FAT32 U 盘：

* WinPE 或安装程序：`EFI/Boot/bootaa64.efi` 及其同目录文件、`sources/boot.wim`；
* 已经装好的 Windows：`EFI/Microsoft/Boot/bootmgfw.efi` 和 `EFI/Microsoft/Boot/BCD`。

SimpleInit 会扫描 `EFI/Microsoft/Boot/bootmgfw.efi` 并把 Windows 列进菜单
（见 [UEFI 控制台](boot/uefi-console.md#内置菜单与自动保存)）。没有出现在菜单里时，
选 **从文件启动**，直接选 U 盘上的 `EFI/Boot/bootaa64.efi` 或
`EFI/Microsoft/Boot/bootmgfw.efi`。UEFI Shell 也能用同样的路径手工启动。

USB 主机控制器在这版固件里保持已初始化，Windows 交接时不会被拆掉。交接判定
按 `winload.efi`、`bootmgfw.efi`、`bootmgr.efi` 三个镜像名，日志出现
`PIANO_LATE_HANDOFF_TARGET target=windows keep=1` 说明走了 Windows 路径。

## 安装到哪里

内部 UFS 由固件以只读方式发布（见 [UEFI 核心](../devel/uefi-core.md#ufs-写入)），
Windows 安装程序不能分区或写入内置存储。验证阶段把 Windows 装到外接 U 盘或
USB 硬盘：

1. 用一个 FAT32 分区放 EFI 文件，其余空间留给 Windows。
2. 从 **从文件启动** 进入 `bootaa64.efi`，在安装程序里选择外接盘。
3. 安装完成后从固件菜单或 **从文件启动** 选外接盘上的
   `EFI/Microsoft/Boot/bootmgfw.efi`。

这样不会改动 Android 和 `sunuefi_esp`／`sunuefi_root`。内置存储安装要等固件
提供可写的块设备，并和 Android 的使用做好仲裁，目前没有实现。

## 失败后回 Android

每一步都不依赖上一步成功。

| 卡在哪里 | 怎么做 |
| --- | --- |
| `fastboot boot` 后没有 UEFI | 电脑上 `fastboot reboot`，等 Android 正常开机。临时启动没有写任何分区 |
| 有 `SunUEFI-piano`，但屏幕白屏 | `fastboot -s SunUEFI-piano getvar product` 确认固件还在；再 `fastboot -s SunUEFI-piano reboot`。看到开机选择页时用音量键选 Android |
| 完全无响应 | 长按电源键强制重启，用你已知的原厂强制重启方式 |
| Windows 介质没进菜单 | 重启回菜单，确认 U 盘是 FAT32、路径是 `EFI/Boot/bootaa64.efi`；也可以用 **从文件启动** |
| Windows 安装中断、蓝屏或反复重启 | 拔掉外接盘后强制重启。临时启动时会回到 Android；装了模块时在助手 **启动** 页选 Android，或按[故障恢复](../user/recovery.md#用命令改启动路线)用 `piano-boot-request` 改路线 |
| 装了模块后起不来 | 长按电源键；仍然不行时用[合体 BOOT](boot/combined-boot.md)的还原工具恢复原厂 BOOT，或先回 Android 再处理 |

返回 Android 的常规命令：

```sh
fastboot -s SunUEFI-piano reboot
```

Android 里没有助手时：

```sh
BOOT=/dev/block/by-name/boot$(getprop ro.boot.slot_suffix)
piano-boot-request set --device "$BOOT" --target android
```

## 这次查不了的内容

* WinPE 里的设备管理器能否列出 8 个处理器、UFS 和 xHCI，要上机看；本仓库没有
  Windows 镜像，产物里的表签名只能证明表被装进固件，不能证明 Windows 的驱动
  已经匹配。
* Windows 没有触屏（SPI）、GPU、Wi-Fi／蓝牙、音频、摄像头、传感器和充电驱动，
  原因见 [ACPI 表](acpi.md#这次不做的子系统)。Windows 路径不拆触屏控制器，
  但系统没有驱动，只能用 USB 键盘和鼠标。
* 内置存储上装 Windows 不可行，因为 UFS 只读；这条限制与 ACPI 无关。
