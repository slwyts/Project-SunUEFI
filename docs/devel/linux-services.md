# Linux 服务源码地图

平板上运行的硬件服务来自本仓库与固定版本的上游 `debian-piano`。修改服务时，要同时看 unit 的 `ExecStart` 和对应脚本的构建来源；设备上的 `/usr/lib/piano/` 是安装结果。

| 要修改的内容 | 源码位置 | 安装位置或构建入口 |
| --- | --- | --- |
| 显示、触屏、键盘、无线、DSP、音频、视频服务的 unit | [`linux/bsp/common/usr/lib/systemd/system/`](../../linux/bsp/common/usr/lib/systemd/system/) | 配置包安装到 `/usr/lib/systemd/system/` |
| 上述服务调用的 `display-start`、`touch-start`、`keyboard-start` 等脚本原版 | [`upstream/debian-piano-current/rootfs/overlay/usr/lib/piano/`](../../upstream/debian-piano-current/rootfs/overlay/usr/lib/piano/) | 经本地适配后安装到 `/usr/lib/piano/` |
| 启动脚本的本地适配 | [`tools/stage_piano_ram_hardware.py`](../../tools/stage_piano_ram_hardware.py) | `adapt()` 修改固定上游脚本，`build()` 生成并可安装派生文件 |
| 本地 DMA 状态读取与启动准备 helper | [`linux/userspace/piano-ram-hardware-prepare`](../../linux/userspace/piano-ram-hardware-prepare) | 由上述适配工具安装到 `/usr/lib/piano/`；也用于磁盘根系统，名字保留自早期 RAM 启动 |
| 触屏帧解析与输入上报 | [`upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c`](../../upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c) | 编译为 `/usr/bin/piano-touch-view`，由 `piano-touch.service` 启动 |
| 触屏程序的本地观测补丁 | [`tools/patches/piano-touch-view-observability.patch`](../../tools/patches/piano-touch-view-observability.patch) | `build_piano_runtime_helpers.py` 派生源码，`build_release_helpers.py` 编译 |
| 相机服务与图像桥接程序 | [`linux/bsp/optional/camera/`](../../linux/bsp/optional/camera/) 与 [`upstream/debian-piano-current/camera/piano-camerad.c`](../../upstream/debian-piano-current/camera/piano-camerad.c) | 可选配置包安装 unit；helper 构建工具编译 `/usr/lib/piano/piano-camerad` |
| 音频 UCM、模块加载参数、udev 配置 | [`linux/bsp/common/`](../../linux/bsp/common/) | 按目录布局安装；文件清单在 `linux/bsp/manifest.json` |
| Linux 调试入口与引导脚本 | [`linux/userspace/`](../../linux/userspace/) | 包含 `piano-debug-bootstrap`、debug/serial unit、磁盘与 RAM 引导脚本 |
| 内核配置与板级设备树 | [`linux/configs/`](../../linux/configs/) 与 [`linux/dts/`](../../linux/dts/) | 内核源码位于 `upstream/linux-piano/`，补丁来源见内核角色说明 |

例如，`piano-keyboard.service` 调用 `/usr/lib/piano/keyboard-start`。它的原版脚本在上游 overlay，本地等待参数由 `stage_piano_ram_hardware.py` 添加，DMA 状态读取函数则在 `linux/userspace/piano-ram-hardware-prepare`。热更新设备后，修复仍必须保存在这些构建输入中，下一次制作 rootfs 才能保留。

键盘背光使用内核 LED 节点 `nanosic::kbd_backlight`，经 UPower 和桌面电源组件调节。`linux/bsp/common/usr/lib/systemd/system/upower.service.d/20-piano-keyboard.conf` 让 UPower 在键盘初始化后启动；release 适配工具也读取这同一份文件，不另写一套调光服务。

当前相机服务在用户会话开始前创建 V4L2 loopback 节点，并由 `piano-camerad` 发送 systemd 就绪通知。真实相机驱动仍由 `piano-camera.service` 在显示、触屏初始化后加载；应用请求视频时才启动采集。`CAMERA_START=0` 同时禁用这两部分。升级时需要一起更新 unit 和 helper，旧程序不发送就绪通知，不能与新的 `Type=notify` unit 混用。

## 两条 rootfs 构建路径

通用多发行版路径由 [`tools/assemble_rootfs.py`](../../tools/assemble_rootfs.py) 安装发行版、桌面和显式选择的 BSP/运行时包；[`tools/package_bsp.py`](../../tools/package_bsp.py) 按 `linux/bsp/manifest.json` 打包本地配置。配置包本身不自动启用服务，也不包含内核模块或原生程序。

现有 Debian GNOME release 路径由 [`tools/build_release_rootfs.py`](../../tools/build_release_rootfs.py) 调用上游 rootfs 构建，随后依次运行 `stage_piano_full_userspace.stage()`、`stage_piano_ram_hardware.build()`、内核模块安装和原生 helper 安装，再处理磁盘启动策略。其基础 unit 与服务启用链接来自上游 overlay，**没有通过 `package_bsp.py` 安装整个本地配置包**。因此只修改 `linux/bsp/common/`，不能假定 release 路径一定包含改动；需要核对对应安装步骤。

## 仍需收敛的地方

本地适配仍在构建时用 Python 修改固定上游脚本，完整的派生脚本没有作为源码直接存放在 `linux/`；通用 BSP 与 Debian release 的配置安装入口也尚未完全统一。这两处应逐步合并为同一份运行时补丁及安装来源。迁移时比较实际生成文件，保留当前能运行的服务行为。

旧 `bootprofiles/` 的源码已分别迁入 `uefi/` 和 `linux/`。该目录本地剩余的 `BootShim.bin`、`BootShim.elf` 与 `__pycache__` 是历史编译产物，当前工具使用 `uefi/handoff/bootshim/`，不从旧目录构建。
