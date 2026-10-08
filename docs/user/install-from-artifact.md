# 下载包中的安装入口

从 GitHub Actions 的 **Build products** 下载并解压 `piano-目标-提交号`。包里有 `install.sh`（Linux / macOS）、`install.cmd`（Windows）、独立的安装器和 `INSTALL.md`。不需要克隆仓库。

| 目标 | 内容 |
| --- | --- |
| `uefi` | 固件镜像和安装工具，没有 ESP / root 镜像 |
| `linux` | Linux 内核与模块构建结果 |
| `debian-gnome` | 完整的 Debian / GNOME 系统包，放在 `bundle/` 里，可用来更新磁盘 |

构建日志在另一个 `piano-build-records-目标-提交号` 里。

## 需要什么

* 电脑：Python 3.10+，Android platform-tools（`adb`、`fastboot` 在 PATH 中）。可以用 `SUNUEFI_PYTHON` 指定 Python。
* 平板：已解锁 Bootloader、已 root、开启 USB 调试，进入 Android。
* 平板里已有 `sunuefi_esp` 和 `sunuefi_root` 两个分区。安装器不能给全新平板分区，会返回 `NEW_INSTALL_NOT_READY`。

## 步骤

先用 `adb devices -l` 找到 Android 下的序列号（不是 `SunUEFI-piano`）。以下三条命令只读取，不写入：

```sh
sh install.sh inspect --serial 序列号 --output inspect.json
sh install.sh plan --serial 序列号 --output update-plan.json
sh install.sh apply --serial 序列号 --plan update-plan.json
```

Windows 把 `sh install.sh` 换成 `install.cmd`。完整磁盘包会自动使用旁边的 `bundle/`；`uefi` 包没有磁盘镜像，需要时用 `--bundle 绝对路径` 指定。

确认计划后，加上 `--execute` 才会写入：

```sh
sh install.sh apply --serial 序列号 --plan update-plan.json --execute
```

## 可选：启用 SSH 登录

如果希望从电脑远程管理 Linux，可以在安装时提供自己的 SSH **公钥**。安装器会把它加入平板的 `root` 和 `piano` 账户；不提供公钥也可以正常进入桌面。

创建安装计划和执行安装时，需要指定同一个 `.pub` 文件：

```sh
sh install.sh plan --serial 序列号 --ssh-public-key ~/.ssh/id_ed25519.pub --output update-plan.json
sh install.sh apply --serial 序列号 --plan update-plan.json --ssh-public-key ~/.ssh/id_ed25519.pub --execute
```

这项功能需要 Linux 电脑上的 `e2fsprogs` 和 GNU `coreutils`。Windows 使用 `install.cmd`，并在默认 WSL 发行版中安装这两个软件包。请只提供公钥，私钥始终留在自己的电脑上；发布镜像不包含任何人的登录密钥。

## 它会做什么

安装器核对 GPT、设备身份、分区容量和镜像哈希，只更新 `sunuefi_esp` 和 `sunuefi_root`，然后回到 Android 读回检查。接着读出本机的原厂蓝牙地址，写入 ESP 里 Linux `boot.img` 的设备树，让蓝牙控制器有自己的地址。通用包里没有这个地址，所以安装后 ESP 与下载包不再逐字节相同，这是正常的。

已经装好相同的系统、只想补上蓝牙地址时：

```sh
sh install.sh provision-bluetooth --serial 序列号
sh install.sh provision-bluetooth --serial 序列号 --execute --output device-provision.json
```

## 不做什么

* 不给新平板分区、不缩小 userdata。
* 不写 BOOT 分区。合体 BOOT 需要另外生成和写入，见[原生 BOOT 重打包](../devel/android-boot-repack.md)。
* 不支持 `--recovery`，会被拒绝，原因见[返回 Android](recovery.md)。
* 出现问题时的退路见[返回 Android 与故障恢复](recovery.md)。
