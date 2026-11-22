# 从电脑刷写镜像

这是备用方式：在电脑上用原厂 Fastboot 更新平板里已有的 `sunuefi_esp` 和 `sunuefi_root` 分区。日常安装请用平板上的引导助手，步骤见 https://github.com/slwyts/Project-SunUEFI/blob/main/docs/user/install-from-artifact.md 。

> [!WARNING]
> 刷写 `sunuefi_root` 会覆盖独立系统里的所有文件，先备份自己的数据。不要把这些镜像刷进 Android 的其他分区。

## 准备

* 平板已解锁 Bootloader，并且用引导助手创建过独立分区，也就是已有 `sunuefi_esp` 和 `sunuefi_root`。没有的话先在助手的 **存储** 页点 **创建独立分区**。
* 电脑装有 Android platform-tools（`adb` 和 `fastboot`），并有一根 USB-C 数据线。
* 按[下载说明](https://github.com/slwyts/Project-SunUEFI/blob/main/docs/user/install-from-artifact.md#下载)获取同一版本的 `SunUEFI-ESP.zip` 和 `SunUEFI-Root-发行版.zip`，解压得到 `esp.sparse.img` 和 `root-发行版.sparse.img`。它们是 Android 稀疏镜像，在本流程中交给原厂 Fastboot 刷写，不要直接用 `dd`。

## 刷写

1. 在 Android 里打开 USB 调试，连接电脑，进入原厂 Fastboot 并查看设备序列号：

   ```sh
   adb reboot bootloader
   fastboot devices
   ```

2. 把下面命令里的 `SERIAL` 换成上一步看到的序列号，root 文件名换成下载的发行版：

   ```sh
   fastboot -s SERIAL -S 128M flash sunuefi_root root-debian-gnome.sparse.img
   fastboot -s SERIAL -S 128M flash sunuefi_esp esp.sparse.img
   fastboot -s SERIAL reboot
   ```

引导本身仍通过平板上的助手安装和维护，这里不处理。

## 怎样确认成功

平板重启回到 Android 后，在助手的 **启动** 页选 **UEFI** 和 **Linux**，保存并重启。Linux 首次启动会自动把根文件系统扩展到分区大小，蓝牙地址由系统自动读取。登录用户名是 `piano`，密码是 `piano123456`。

## 出问题怎么办

启动失败或想回 Android，见 https://github.com/slwyts/Project-SunUEFI/blob/main/docs/user/recovery.md 。
