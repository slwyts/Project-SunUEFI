# RAM-root Linux 发行版调试 bootstrap

bootprofiles/linux-userspace 提供可复制到BusyBox initramfs或Debian ARM64 RAM root的POSIX shell脚本。默认只输出可观察diagnostics，不启用gadget、交互shell或恢复timer。它不修改kernel pins，不挂载Android/data，不调用insmod/modprobe，不创建USB存储功能，也不改role/PHY/TCSR/devmem。

## 文件与调用

把 piano-debug-bootstrap 与 piano-debug-stage 安装到RAM root的 /usr/local/sbin 并保留执行权限。调用者须先提供proc/sys/dev/run；这些脚本不接管发行版PID1，也不自动挂载块设备。

- `piano-debug-bootstrap diagnose`：读cmdline、kernel、CPU、现有UDC/state/function/speed与role；默认入口，无配置副作用。
- `piano-debug-stage`：先diagnose，再处理显式恢复参数，再start；保留清晰失败退出码。
- `piano-debug-bootstrap start`：仅在cmdline明确开启时建立标准configfs ACM+NCM。
- `piano-debug-bootstrap serial`：仅在显式shell参数、真实ttyGS0字符设备以及本次boot拥有的gadget成立后启动agetty/BusyBox getty。
- `piano-debug-bootstrap stop`：只解除本boot拥有且binding未被替换的gadget，不删除其他gadget。

Debian可选择随附systemd oneshot debug unit与显式serial unit。serial unit还有ConditionKernelCommandLine，不会在默认关shell时反复启动。输出进入journal/console；交互后可用dmesg或journalctl查看Linux日志。initramfs可直接调用stage，并由自己的supervisor管理serial命令。bootstrap不安装/启动SSH，不修改密码或配置host网络。

## 明确配置与 kernel commandline

完整公开内核保留 `CONFIG_CMDLINE_FORCE=y`。外部 EFI load options 因此不能
覆盖编译命令行中的调试参数。RAM 发行版可用 `/etc/piano/linux-debug.conf`
明确配置以下键；脚本逐键读取纯数据，不 source/eval，kernel cmdline 同名项优先：

```
usb=acm-ncm
shell=1
ipv4=192.168.77.1/30
recovery_seconds=0
```

`udc` 和 `role` 可设置为实机存在的确切名称，省略时仍必须唯一且关联验证
成功。serial unit依靠实际脚本检查显式shell选择及当前boot gadget归属，配置
文件与命令行两种方式均可用。默认没有文件且没有参数时仍只做诊断。

```
piano.debug_usb=acm-ncm
piano.debug_udc=CONTROLLER_NAME
piano.debug_role=RELATED_ROLE_SWITCH
piano.debug_shell=1
piano.debug_ipv4=192.168.77.1/30
piano.recovery_seconds=180
```

UDC/role可省略，但只有唯一、可验证关联的实例才自动选中。IPv4可省略；设定时只为新NCM interface增加192.168.77.1/30并使link up，不设置default route、DHCP、forwarding或host地址。电脑端可手动配192.168.77.2/30，实际可连通性单独验证。

shell为本地实验显式root autologin；不加该参数不开放shell。recovery_seconds默认0，不启timer；只有 `recover` /stage明确处理1..86400范围的参数后才在RAM /run保存timer PID并调用Linux reboot。恢复覆盖Linux用户态已启动的阶段，不保证EFI/内核早期卡住可恢复。不要把timer已创建计为设备重启验证。

## 真实能力验证与拒绝条件

配置操作要求root及rootfs/tmpfs/ramfs根文件系统。非RAM根或/run状态目录挂在持久文件系统时拒绝配置，diagnose仍可用。CONFIGFS必须在实际/proc/filesystems中可用，mount也只能是configfs。已加载/内建libcomposite须提供usb_gadget目录；脚本不盲加载模块。创建acm/ncm函数由标准configfs/kernel完成，函数不支持时失败并只回滚自己创建的目录。

UDC来自实际/sys/class/udc，有state/function元数据；已有gadget/function、未知state、未知provider或重复选择均拒绝。当前只接受已存在的dwc3 driver。通过device symlink确认UDC与role switch属于同一控制器层级，实际role必须是device，bind之前再次读回。role未知、host/none或无关联证据时只记录失败，不硬切Type-C/PHY。未来不暴露usb_role的provider需新增准确只读证据适配，不能用授权boolean绕过检查。

configfs gadget使用实验开发ID1209:8751、CDC IAD、ACM和NCM功能、确定的本地MAC。它不代表正式注册量产USB ID。成功写UDC后日志只报告 `gadget_bound` 和 `enumeration_verified=0`；真实host枚举、tty交互、NCM传输及Piano PHY/role由实机证据证明。系统自带udev/内建storage driver行为属于kernel/distro，不能据本脚本不autoload模块声称整个发行版绝不访问UFS；本脚本自身不mount数据分区。

## 验证范围

tests/test_linux_debug_bootstrap.py运行实际shell脚本，以临时proc/sys/configfs边界与命令fixture测试16个场景：默认无操作/timer、ACM+NCM配置、自己拥有的stop、配置失败回滚、UDC/role/provider/state/configfs拒绝、路径参数、非RAM root、已有gadget保留、显式timer、显式NCM地址无路由及shellopt-in。fixture状态全部打印evidence=fixture，不计Piano枚举成功。

标准依据：[Linux gadget configfs](https://docs.kernel.org/usb/gadget_configfs.html)；[UDC sysfs ABI](https://raw.githubusercontent.com/torvalds/linux/master/Documentation/ABI/stable/sysfs-class-udc)；[USB role switch ABI](https://raw.githubusercontent.com/torvalds/linux/master/Documentation/ABI/testing/sysfs-class-usb_role)。这些接口提供Linux标准功能，不能取代SM8750硬件后端的验证。
