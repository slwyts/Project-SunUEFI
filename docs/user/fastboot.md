# UEFI Fastboot

UEFI Fastboot 是 SunUEFI 运行期间在后台提供的 USB Fastboot 服务，用来读日志、截图、只读读取分区和请求启动。屏幕白屏、没有菜单时也能用。固件把控制权交给 Linux 时，它随之退出。

## 和另外两个 Fastboot 的区别

| 名称 | 设备序列号 | 什么时候出现 | 刷写 |
| --- | --- | --- | --- |
| 原厂 Bootloader Fastboot | 平板真实序列号 | `adb reboot bootloader` | 可以 |
| UEFI Fastboot | `SunUEFI-piano` | SunUEFI 运行期间 | 拒绝 |
| Linux 的 USB 网络 / SSH | 无 | Linux 启动之后 | 不适用 |

下面所有命令都带 `-s SunUEFI-piano`，确保操作的是 UEFI 服务，而不是原厂 Bootloader。服务在 UEFI 菜单、固件设置和 Shell 里一直保持连接，切换页面不会退出。

## 查看状态

```sh
fastboot -s SunUEFI-piano getvar product        # piano-sunuefi
fastboot -s SunUEFI-piano getvar version
fastboot -s SunUEFI-piano getvar storage-policy
fastboot -s SunUEFI-piano oem status
```

`oem status` 还会输出最近一次设备树修正的失败阶段、冷启动内存检查结果，以及最近一次日志快照的状态和缓冲区头。日志无法导出时也可以读取这些信息；查询不会重新探测硬件或改变启动条件。

## 日志和截图

```sh
fastboot -s SunUEFI-piano oem ramlog
fastboot -s SunUEFI-piano get_staged uefi-ramlog.txt      # 最多 256 KiB

fastboot -s SunUEFI-piano oem screenshot
fastboot -s SunUEFI-piano get_staged uefi-screenshot.bmp  # 24 位 BMP
```

截图读取的是帧缓冲，不一定是物理屏幕此刻显示的内容。

## 读取分区

```sh
fastboot -s SunUEFI-piano fetch 分区名 文件
```

只读。单次请求最多 64 KiB，fastboot 工具会自动分块。

## 启动与退出

| 命令 | 作用 |
| --- | --- |
| `oem boot-stable` | 读取 ESP 里的 `\EFI\Piano\stable\boot.img`，启动 Linux |
| `oem setup`、`oem shell`、`oem simpleinit` | 切换到 UEFI 设置、Shell 或 SimpleInit 菜单 |
| `reboot` | 释放 USB、存储、时钟和 SMMU 后重启，沿用已保存的启动路线 |
| `continue` | 同样重启，不改变启动选择，也不会直接启动 Linux |
| `stage 文件` | 把文件下载到内存，最大 64 MiB |
| `oem sha256`、`oem discard` | 对暂存的数据计算哈希，或丢弃它 |

## 不支持的命令

`flash`、`erase`、`flashing`、切换槽位和未知的 `oem` 命令一律被拒绝。读日志用 `oem ramlog`，`oem log` 会失败。`boot 文件` 还不是通用的系统或 EFI 启动入口。

电脑看不到 `SunUEFI-piano` 时，见[故障恢复](recovery.md)。
