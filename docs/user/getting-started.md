# 快速上手指南

本指南说明如何用电脑临时加载 SunUEFI。当前仍是开发候选，最新构建未实机测试；Linux 体验还需要事先准备专用分区、启动文件和匹配模块，下面的命令不会自动安装 Linux。

---

## 准备工作

1. **设备准备**：
   * 小米平板 8 Pro (`piano`)，已在官方 Fastboot 下解锁 Bootloader（`unlocked: yes`）。
   * 建议提前备份平板中的重要个人数据。
2. **电脑环境**：
   * 电脑端已安装 `adb` 与 `fastboot` 命令行工具。
   * Type-C 数据线。
3. **固件镜像**：
   * 统一产品镜像文件：`PianoUEFI-product.img`。
   * 自行编译请参考[开发者构建手册](../devel/building.md)，生成文件位于 `artifacts/product/PianoUEFI-product.img`。
   * 运行 `sha256sum PianoUEFI-product.img`，与提供这份镜像时附带的 SHA256 比较。

---

## 步骤一：连接设备并进入 Fastboot

1. 平板开机进入 Android，在“开发者选项”中打开“USB 调试”。
2. 使用数据线连接电脑，在终端中执行：
   ```sh
   adb reboot bootloader
   ```
3. 检查设备连接与解锁状态：
   ```sh
   fastboot devices
   fastboot getvar unlocked
   ```
   确认设备已列出且 `unlocked: yes`。

---

## 步骤二：临时加载固件

使用 Android 标准的 `fastboot boot` 命令将固件临时载入内存运行。该加载命令不刷写启动分区；运行中的固件和 Linux 各有自己的存储行为。

在存放固件的目录下执行：

```sh
fastboot boot PianoUEFI-product.img
```

终端返回 `Sending 'boot' ... OKAY` 和 `Booting ... OKAY` 表示 Bootloader 接受了请求，接下来还要确认固件实际运行。

---

## 步骤三：启动现象与连接确认

1. **关于白屏**：
   * 当前集成固件可能白屏，原因仍在调查。白屏本身不能说明后台是否正常，先查询 USB 连接。
2. **验证后台连接**：
   * 固件启动后，会向电脑暴露名为 `SunUEFI-piano` 的 USB 调试设备。在终端中执行：
     ```sh
     fastboot -s SunUEFI-piano getvar product
     ```
   * 若返回 `product: piano-sunuefi`，说明 UEFI 已成功运行并建立连接。

---

## 步骤四：引导 Linux 或返回 Android

### 选项 A：引导进入已安装的 Linux
若设备已按照部署说明划分了专用分区并放置了引导文件，输入：

```sh
fastboot -s SunUEFI-piano oem boot-stable
```

* 固件会读取 ESP 的 `\EFI\Piano\stable\boot.img` 并启动。
* 最近一次约 130 秒进入 GNOME，Linux 原生显示驱动接管后屏幕恢复显示；这不是所有候选的启动时间保证。

### 选项 B：退出并返回 Android
体验完毕或需返回原厂系统时，输入：

```sh
fastboot -s SunUEFI-piano reboot
```

正常退出路径会释放资源并冷重启回 Android。若命令无响应，按下方恢复页处理。原厂 Recovery 已在此前实验后恢复。

---

## 相关页面

* 遇到卡死或无响应时的强制重启方法：[安全返回 Android](recovery.md)
* 提取 RAM 日志与显存截图：[Fastboot 命令速查](fastboot.md)
* 白屏机制与启动耗时细节：[已知问题解答](known-issues.md)
