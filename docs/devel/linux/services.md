# Linux 服务源码地图

平板上运行的硬件服务来自本仓库与固定版本的上游 `debian-piano`。修改服务时，要同时看 unit 的 `ExecStart` 和对应脚本的构建来源；设备上的 `/usr/lib/piano/` 是安装结果。

| 要修改的内容 | 源码位置 | 安装位置或构建入口 |
| --- | --- | --- |
| 触屏和 USB 网络的 unit | [`linux/bsp/common/usr/lib/systemd/system/`](../../../linux/bsp/common/usr/lib/systemd/system/) | 配置包安装到 `/usr/lib/systemd/system/` |
| 共用硬件运行时安装 | [`tools/stage_piano_ram_hardware.py`](../../../tools/stage_piano_ram_hardware.py) | 读取 BSP 清单、保留权限和符号链接，清理旧的按序加载服务 |
| DMA 状态诊断 | [`linux/userspace/piano-ram-hardware-prepare`](../../../linux/userspace/piano-ram-hardware-prepare) | 仅用于显式诊断，不阻止模块自动探测 |
| 触屏帧解析与输入上报 | [`upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c`](../../../upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c) | 编译为 `/usr/bin/piano-touch-view`，由 `piano-touch.service` 启动 |
| 触屏程序的本地观测补丁 | [`tools/patches/piano-touch-view-observability.patch`](../../../tools/patches/piano-touch-view-observability.patch) | `build_piano_runtime_helpers.py` 派生源码，`build_release_helpers.py` 编译 |
| 相机服务、图像处理与控制接口 | [`linux/bsp/optional/camera/`](../../../linux/bsp/optional/camera/) 与 [`linux/userspace/camera/`](../../../linux/userspace/camera/) | 共用 Makefile 编译服务与客户端，两条运行库构建入口使用同一份源码 |
| 音频 UCM、模块加载参数、udev 配置 | [`linux/bsp/common/`](../../../linux/bsp/common/) | 按目录布局安装；文件清单在 `linux/bsp/manifest.json` |
| Linux 调试入口与引导脚本 | [`linux/userspace/`](../../../linux/userspace/) | 包含 `piano-debug-bootstrap`、debug/serial unit、磁盘与 RAM 引导脚本 |
| GNOME 电源键与保护套合盖 | [`linux/desktops/gnome/power-overlay/`](../../../linux/desktops/gnome/power-overlay/) | 复用 `piano-power-button.service`，完整 stager 和 GNOME 组装安装同一份源码；当前只控制背光，锁屏/睡眠限制见[说明](../hardware/power-button.md) |
| 内核配置与板级设备树 | [`linux/configs/`](../../../linux/configs/) 与 [`linux/dts/`](../../../linux/dts/) | 内核源码位于 `upstream/linux-piano/`，补丁来源见内核角色说明 |

硬件驱动通过 OF、platform、SPI、RPMsg 等标准 modalias 加载，依赖由设备树、内核 supplier 和延迟探测表达。显示、键盘、无线、音频和视频没有按加载顺序运行的用户空间服务。热更新定位后的修复必须写回内核、设备树或对应程序源码。

键盘背光使用内核 LED 节点 `nanosic::kbd_backlight`，经 UPower 和桌面电源组件调节。蓝牙地址由 UEFI 通过标准设备树属性交给内核，触屏解码由设备绑定事件触发。ADSP 和 CDSP 由内核 `remoteproc` 驱动自动启动，设备树不再禁用 ADSP 自动启动，也不依赖用户空间等待显示接管。

相机服务在用户会话开始前创建 V4L2 loopback 节点，并发送 systemd 就绪通知；只有应用请求视频时才启动采集。相机驱动由硬件别名自动探测，`CAMERA_START=0` 禁用图像服务。升级时一起更新 unit 和 helper。

2026-10-08 的 `g086a94c4529d` 实机已绑定主线接口驱动 `qcom-camss`、后置 `s5kjn1 7-0010`、前置 `ov32d40 5-0010` 和对焦 `dw9768 7-000c`。原厂最终 DT 与现有 [相机 overlay](../../../upstream/debian-piano-current/boot/dtbo-piano-camera.dts) 对应：后置走 CCI0 master1、GPIO115/116、CSIPHY1；前置走 QUP2 SE1 I²C、GPIO4/5、CSIPHY4。内核的 `drivers/media/platform/qcom/camss/` 已包含 SM8750、CSID980 和 TFE Gen4 支持，两个 sensor 位于 `drivers/media/i2c/{s5kjn1,ov32d40}.c`。这不是重新移植缺失驱动的场景；`video40/41` 是 loopback，不能单凭枚举判定有图像。此版本经普通 BOOT 启动后，两路各做了30帧短采集，硬件各输出28帧；前后预览已检查，未再出现 CCM `Bad address`。后置等待90帧后画面比初始预览更清晰。长期帧率、同时采集和 GNOME 应用体验仍未验证。

相机硬件当前只交给 `piano-camerad` 管理；它配置 CSID→TFE PIX、缩放和 NV12，并提供曝光、白平衡、对焦控制。[WirePlumber 配置](../../../linux/bsp/optional/camera/etc/wireplumber/wireplumber.conf.d/50-piano-camera.conf) 因此禁用 `monitor.libcamera` 和直连 CAMSS 节点，应用使用 loopback。标准 [libcamera v0.7.1 simple pipeline](https://gitlab.freedesktop.org/camera/libcamera/-/blob/v0.7.1/src/libcamera/pipeline/simple/simple.cpp) 有通用 CAMSS/software ISP 支持，但尚未接入这里的 TFE 硬件 ISP 路径；不要同时开启两个管理程序争用传感器。若以后改用 libcamera，需要先完成 media 路径、硬件控件及图像调校的接入。

相机3A指自动曝光、白平衡和对焦，当前这部分必要算法由 `piano-camerad` 使用内核 sensor/ISP 控件实现。前摄自然光日志中，高亮像素占比在3%..8%间变化；旧 AE 跨过4%时会突然降约19%曝光量，又因平均亮度不足而提高增益。稳定性补丁改为连续高亮响应、每相机统计滤波、约5%亮度误差死区和单次6%变化上限，保留既有曝光/增益分配、白平衡与对焦。用户确认现场是自然光、没有开室内灯，因此没有强制新增50Hz防闪烁参数。

修正后的 ARM64 camerad 已部署。本机前摄的一次900帧采集中，启动3秒后的810帧全画面 YAVG 为76.54..77.76，曝光和两项增益保持2808/15872/6958；程序自身的局部亮度统计为93..95，采样范围不同，不能直接比较这两组亮度的绝对值。采集只保留统计，没有保存相机视频。这确认了该场景下控制循环稳定，尚未测量光照突变和完整模式切换，也不代表完整原厂3A调校。原始统计在本地 `内部记录目录

## 两条 rootfs 构建路径

通用多发行版路径由 [`tools/assemble_rootfs.py`](../../../tools/assemble_rootfs.py) 安装发行版、桌面和显式选择的 BSP/运行时包；[`tools/package_bsp.py`](../../../tools/package_bsp.py) 按 `linux/bsp/manifest.json` 打包本地配置。配置包本身不自动启用服务，也不包含内核模块或原生程序。

Debian release 由 [`tools/build_release_rootfs.py`](../../../tools/build_release_rootfs.py) 调用上游基础构建，再用 `stage_piano_ram_hardware.build()` 安装经过同一 BSP 清单校验的本地配置，删除上游的旧驱动加载服务，随后安装匹配的模块与原生程序。SteamOS 和 Nyarch 通过 `stage_arch_piano.py` 使用相同硬件运行时。

旧 `bootprofiles/` 的源码已分别迁入 `uefi/` 和 `linux/`。该目录本地剩余的 `BootShim.bin`、`BootShim.elf` 与 `__pycache__` 是历史编译产物，当前工具使用 `uefi/handoff/bootshim/`，不从旧目录构建。
