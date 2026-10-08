# 下载包中的安装入口

解压 GitHub 构建产物后，Linux 使用 `install.sh`，Windows 使用 `install.cmd`。安装器是随包复制的独立 Python 文件，不需要克隆仓库。电脑需要 Python 3.10+ 与 Android platform-tools（`adb`、`fastboot` 在 PATH 中）；设备需要已解锁、开启 USB 调试的 rooted Android。

直接运行或双击只显示帮助。开始检查时明确选择 Android ADB 序列号；它不是固件驻留服务的 `SunUEFI-piano`：

```sh
sh install.sh inspect --serial 原机ADB序列号 --output inspect.json
sh install.sh plan --serial 原机ADB序列号 --output update-plan.json
sh install.sh apply --serial 原机ADB序列号 --plan update-plan.json
```

这三步只读取状态、生成计划和再次核对。完整磁盘包会自动使用旁边的 `bundle/`；UEFI-only 包只有固件与工具，没有 ESP/root 镜像，也不会伪造可安装的磁盘 manifest。需要其他磁盘包时显式传 `--bundle /absolute/path`。

确认计划后，只有这一步允许写入：

```sh
sh install.sh apply --serial 原机ADB序列号 --plan update-plan.json --execute
```

Windows 在终端将以上 `sh install.sh` 换成 `install.cmd`。Linux 示例直接通过 `sh` 运行，文件没有执行权限也可使用。脚本可从任意目录调用，支持带空格的解压路径；相对 `--plan`、`--output` 路径以当前终端目录为准。可用 `SUNUEFI_PYTHON` 指定 Python 可执行文件。

当前执行范围是**更新已经存在的 `sunuefi_esp` 与 `sunuefi_root`**。安装器核对 GPT、设备身份、容量、镜像哈希，更新后回 Android 检查 generic 镜像读回。随后只读取得本机原厂蓝牙地址，挂载项目 ESP，更新 Linux `boot.img` 内嵌 DTB 并核对文件 hash；通用包不包含该地址，Windows 不需要 mtools。完成后 ESP 已是本机配置，不再与 generic 镜像逐字节相同，两阶段结果分别记录。安装器不会因脚本名叫 install 就自动建立新平板分区。首次缩小 userdata／更新 GPT 仍未开放；Recovery 安装仍在验证，`--recovery` 会拒绝。构建、主机检查和写入读回不能代替实际 Linux 启动验证。

已有相同 generic 包、只需补齐本机蓝牙 DT 配置时，可先只读检查，再明确执行，无需重刷 root：

```sh
sh install.sh provision-bluetooth --serial 原机ADB序列号
sh install.sh provision-bluetooth --serial 原机ADB序列号 --execute --output device-provision.json
```

该步骤需要正常启动完成的 rooted Android。它只写 `sunuefi_esp` 中的项目文件，保留 Linux kernel/initramfs；source 格式或已有 boot 文件不匹配时会停止。在线安装器和本机地址 DTB 的实际启动仍待设备验证。
