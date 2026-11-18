# Linux 服务源码地图

平板上运行的硬件服务来自本仓库与固定版本的上游 `debian-piano`。修改服务时，要同时看 unit 的 `ExecStart` 和对应脚本的构建来源；设备上的 `/usr/lib/piano/` 是安装结果。

| 要修改的内容 | 源码位置 | 安装位置或构建入口 |
| --- | --- | --- |
| 笔的 BlueZ 初始化和 USB 网络 unit | [`linux/bsp/common/usr/lib/systemd/system/`](../../../linux/bsp/common/usr/lib/systemd/system/) | 配置包安装到 `/usr/lib/systemd/system/` |
| 共用硬件配置 | [`linux/bsp/manifest.json`](../../../linux/bsp/manifest.json) | `piano-device-config` 拥有 UCM、笔校准和通用设备规则 |
| 硬件运行时安装 | [`tools/stage_piano_ram_hardware.py`](../../../tools/stage_piano_ram_hardware.py) | 安装相机配置与显式诊断工具，清理旧的按序加载服务 |
| 按需充电记录 | [`linux/bsp/common/usr/lib/piano/charge-monitor`](../../../linux/bsp/common/usr/lib/piano/charge-monitor) | `piano-charge-monitor.service`，经共用 BSP 安装，preset 默认关闭 |
| DMA 状态诊断 | [`linux/userspace/piano-ram-hardware-prepare`](../../../linux/userspace/piano-ram-hardware-prepare) | 仅用于显式诊断，不阻止模块自动探测 |
| 手指帧解码与多点输入 | [`0095-input-nt36532e-matrix.patch`](../../../patches/linux/7.3/0095-input-nt36532e-matrix.patch) | 编入 `nt36532e_ts`，在现有 SPI 中断线程中直接上报标准输入事件 |
| 内核笔解码与 BLE 融合 | [`0123-input-nt36532e-pen.patch`](../../../patches/linux/7.3/0123-input-nt36532e-pen.patch) | 编入 `nt36532e_ts`；从标准固件目录加载数字校准，由物理 SPI tablet-tool 上报输入 |
| 原始帧诊断 | [`upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c`](../../../upstream/debian-piano-current/initramfs/touch-view/piano-touch-view.c) | 派生后编译为 `/usr/bin/piano-touch-view`；由开发者手动运行 |
| 触屏程序的本地观测补丁 | [`tools/patches/piano-touch-view-observability.patch`](../../../tools/patches/piano-touch-view-observability.patch) | `build_piano_runtime_helpers.py` 派生源码，`build_release_helpers.py` 编译 |
| 相机服务、图像处理与控制接口 | [`linux/bsp/optional/camera/`](../../../linux/bsp/optional/camera/) 与 [`linux/userspace/camera/`](../../../linux/userspace/camera/) | 共用 Makefile 编译服务与客户端，两条运行库构建入口使用同一份源码 |
| 音频 UCM、模块加载参数、udev 配置 | [`linux/bsp/common/`](../../../linux/bsp/common/) | 按目录布局安装；文件清单在 `linux/bsp/manifest.json` |
| Linux 调试入口与引导脚本 | [`linux/userspace/`](../../../linux/userspace/) | 包含 `piano-debug-bootstrap`、debug/serial unit、磁盘与 RAM 引导脚本 |
| GNOME 电源键与保护套合盖 | [`linux/desktops/gnome/power-overlay/`](../../../linux/desktops/gnome/power-overlay/) | 复用 `piano-power-button.service`，完整 stager 和 GNOME 组装安装同一份源码；当前只控制背光，锁屏/睡眠限制见[说明](../hardware/power-button.md) |
| 内核配置与板级设备树 | [`linux/configs/`](../../../linux/configs/) 与 [`linux/dts/`](../../../linux/dts/) | 内核源码由 `patches/linux/7.3/series.json` 从公开稳定版重建，补丁来源见内核角色说明 |

手指输入由 `nt36532e` 直接处理，不需要用户空间读取者。矩阵核心与 SPI、固件和输入上报分开，使用整数完成帧校验、基线、行列去噪、局部峰值、接触面积和质心计算；输入层保留十个槽位和掌压锁存。掌压不按下 `BTN_TOUCH`，笔帧取消手指时先报告掌压类型再释放槽位。掌压判定的限制见[笔输入说明](../../../linux/userspace/pen/README.md#对照用的用户态笔输入)。

核心先用 16 个有效帧确定矩阵类型，再对 32 帧逐格取上中位数建立基线。去噪先减每列中位数，再减每行中位数；三路选择超出工作量预算后使用内核排序收尾。原始差值的最大值与最小值相差不到 50 时，去噪后的最大值必然低于 100，可以跳过去噪并执行相同的基线漂移；其余帧在去噪后按触摸阈值 200 决定是否扫描峰值和区域。60×40 的解码状态约 175 KiB，在探测时一次分配，正常帧与恢复路径不再分配内存。

坏校验或截断帧只丢弃，不更新触点或基线；短暂丢样不会中断拖动。固件重载、挂起和移除释放触点，固件重载与挂起后重新学习基线。SPI 传输失败释放当前触点，保留基线；面板恢复时，固件重载失败则保持中断关闭。原始帧 FIFO 只在显式启用捕获时分配，手指输入本身不占用这份 4 MiB 队列。

`piano-device-config` 一起安装 `piano-touch.service` 和它调用的
[`pen-control.py`](../../../linux/bsp/common/usr/lib/piano/pen-control.py)。
服务只通过 BlueZ 初始化厂商 GATT 特征，不读取 THP／hidraw、写 procfs 或创建
uinput。BLE 尚未连接时继续等待；坐标和压感由内核融合，校准也经配置包安装。

`piano-touch-view` 保留原始帧采集和对照功能，不由系统启动服务运行。

硬件驱动通过 OF、platform、SPI、RPMsg 等标准 modalias 加载，依赖由设备树、内核 supplier 和延迟探测表达。显示、键盘、无线、音频和视频没有按加载顺序运行的用户空间服务。热更新定位后的修复必须写回内核、设备树或对应程序源码。

键盘背光使用内核 LED 节点 `nanosic::kbd_backlight`，经 UPower 和桌面电源组件调节。蓝牙地址由 UEFI 通过标准设备树属性交给内核，触屏解码由设备绑定事件触发。ADSP 和 CDSP 由内核 `remoteproc` 驱动自动启动，设备树不再禁用 ADSP 自动启动，也不依赖用户空间等待显示接管。

Debian sid 使用发行版 UPower，不再重建旧版 daemon。签名源码中的
`up-enumerator-udev.c` 已枚举 LED 插拔；`up-daemon.c` 更新组合键盘对象，
保持原 `/org/freedesktop/UPower/KbdBacklight` 接口。Debian 和 Nyarch 的 GSD
共用 51.0 补丁，通过该标准接口异步刷新亮度与范围，取消过期请求，保留
亮度为零的状态，不增加硬件服务。SteamOS 同样使用发行版 UPower。

相机服务在用户会话开始前创建 V4L2 loopback 节点，并发送 systemd 就绪通知；只有应用请求视频时才启动采集。相机驱动由硬件别名自动探测，`CAMERA_START=0` 禁用图像服务。升级时一起更新 unit 和 helper。

2026-10-08 的 `g086a94c4529d` 实机已绑定主线接口驱动 `qcom-camss`、后置 `s5kjn1 7-0010`、前置 `ov32d40 5-0010` 和对焦 `dw9768 7-000c`。原厂最终 DT 与现有 [相机 overlay](../../../upstream/debian-piano-current/boot/dtbo-piano-camera.dts) 对应：后置走 CCI0 master1、GPIO115/116、CSIPHY1；前置走 QUP2 SE1 I²C、GPIO4/5、CSIPHY4。内核的 `drivers/media/platform/qcom/camss/` 已包含 SM8750、CSID980 和 TFE Gen4 支持，两个 sensor 位于 `drivers/media/i2c/{s5kjn1,ov32d40}.c`。这不是重新移植缺失驱动的场景；`video40/41` 是 loopback，不能单凭枚举判定有图像。此版本经普通 BOOT 启动后，两路各做了30帧短采集，硬件各输出28帧；前后预览已检查，未再出现 CCM `Bad address`。后置等待90帧后画面比初始预览更清晰。长期帧率、同时采集和 GNOME 应用体验仍未验证。

相机硬件当前只交给 `piano-camerad` 管理；它配置 CSID→TFE PIX、缩放和 NV12，并提供曝光、白平衡、对焦控制。[WirePlumber 配置](../../../linux/bsp/optional/camera/etc/wireplumber/wireplumber.conf.d/50-piano-camera.conf) 因此禁用 `monitor.libcamera` 和直连 CAMSS 节点，应用使用 loopback。这份配置与服务一起保留，不能在没有可运行替代时单独删除。libcamera 上游支持范围、TFE 统计与逐帧参数接口、前摄时序、并发、时间戳和调校许可的缺项统一见[内核接口与 libcamera](../hardware/camera-controls.md#内核接口与-libcamera)。Debian、SteamOS 和 Nyarch 共用同一内核适配；切换发行版或升级软件包不能补齐这些硬件接口。

相机3A指自动曝光、白平衡和对焦，当前这部分必要算法由 `piano-camerad` 使用内核 sensor/ISP 控件实现。前摄自然光日志中，高亮像素占比在3%..8%间变化；旧 AE 跨过4%时会突然降约19%曝光量，又因平均亮度不足而提高增益。稳定性补丁改为连续高亮响应、每相机统计滤波、约5%亮度误差死区和单次6%变化上限，保留既有曝光/增益分配、白平衡与对焦。用户确认现场是自然光、没有开室内灯，因此没有强制新增50Hz防闪烁参数。

修正后的 ARM64 camerad 已部署。本机前摄的一次900帧采集中，启动3秒后的810帧全画面 YAVG 为76.54..77.76，曝光和两项增益保持2808/15872/6958；程序自身的局部亮度统计为93..95，采样范围不同，不能直接比较这两组亮度的绝对值。采集只保留统计，没有保存相机视频。这确认了该场景下控制循环稳定，尚未测量光照突变和完整模式切换，也不代表完整原厂3A调校。

## 充电记录

记录器只读 power-supply、IIO、hwmon、thermal、Type-C、CPU/GPU 频率和
背光接口。每条 JSON 记录包含 UTC 时间、启动 ID、boottime、采样耗时与
读取错误；数值沿用 sysfs 单位，电源电压、电流为微伏、微安，电源温度为
0.1°C，thermal 与 IIO 温度为毫摄氏度，原始 ADC 保留 scale 和 offset。
`power_w` 从读数计算，PMIC 与电荷泵输入分别保存，合计只作为输入估算，
电池功率正值为净充入、负值为净放电。

在线时每 2 秒采样，离线时每 60 秒采样；netlink 插拔通知提前结束等待，
状态未知时按 2 秒重试。MCA 固件摘要至多每分钟读取一次，插拔时可提前
刷新，认证进行时跳过；不写 debugfs 或启用电荷泵 ADC。一个传感器读取
失败不会丢弃其他数据。系统睡眠不被该服务阻止。

`/var/log/piano-charge-monitor/samples.jsonl` 保存当前记录，每段最多
1 MiB；满段压缩为按序号排列的 `samples-*.jsonl.gz`。当前文件、旧段与
压缩暂存合计不超过 20 MiB，空间不足先删除最早旧段。文件锁防止多个
实例写入同一目录，重启时只截掉未完成的最后一行。服务使用只读系统和
内核接口保护，仅允许日志目录写入。启停与导出见[快速上手](../../user/getting-started.md#充电记录)。

## 两条 rootfs 构建路径

通用多发行版路径由 [`tools/assemble_rootfs.py`](../../../tools/assemble_rootfs.py) 安装发行版、桌面和显式选择的 BSP/运行时包；[`tools/package_bsp.py`](../../../tools/package_bsp.py) 按 `linux/bsp/manifest.json` 打包本地配置。配置包本身不自动启用服务，也不包含内核模块或原生程序。

Debian 基础镜像从签名 APT 源安装 `piano-device-config`，SteamOS 和 Nyarch
从同一个 `sunuefi` pacman 仓库安装。触屏和 USB 服务由包内的 udev 规则启动。
相机配置和诊断程序仍由运行组件组装；产品镜像会检查包拥有的硬件文件没被后续步骤覆盖。

桌面包由 rootfs 构建自动安装，源码与二进制摘要写入发行版 manifest，构建日志
并入 CI 的 `logs.zip`。单独构建使用 `./build.sh gsd --sysroot <Debian root>`、
`./build.sh gdm --sysroot <Debian root>` 和
`./build.sh pipewire --rootfs <发行版 root> --output <build 下的包目录>`；
这些命令需要 root，输入均为本地构建目录，不访问平板。

Debian 从签名 sid 源码重建 PipeWire 的同版包组，保留 Debian 原有拆包和依赖。
相机没有可列出的控件时，WirePlumber 仍能连接视频节点；修正后的 V4L2 采集则由
`libspa-0.2-modules` 提供。旧 Debian 系统使用普通 APT 升级，
`pipewire-v4l2-source` 会变成可删除的空包；原插件和加载配置会一起移除。
新镜像不装这个过渡包，也不默认安装测试或开发包。
从原先只替换插件的版本升级前，可用 APT 下载现装的整组软件包保留回退副本；
Debian sid 更新后，官方源可能不再提供对应的旧版服务端和库。

Nyarch 和 SteamOS 仍独立重建 GStreamer 插件和 `pipewire-v4l2-source`，
使用目标系统同版本的服务端和库，不替换音频组件。补丁处理格式枚举与首选格式、
运行中 caps、缓冲池锁顺序、借用 MemPtr 的复制，以及录像持有的 V4L2 复制帧；
模块选择与缓冲管理见[相机接口](../hardware/camera-controls.md#帧时间戳与错误)。
`patches/pipewire/<版本>/series.json` 记录来源和补丁摘要，源码完整准备后才进入缓存，构建器
变化使旧包缓存失效。19 项格式测试串行运行，使用隔离的 GStreamer registry，
不扫描系统插件；它们不替代平板上的相机、录屏和屏幕共享验证。

GDM 在真实 KMS 设备提供显示接口后启动会话。设备树的临时帧缓冲按平台驱动识别，
不只检查 `ID_PATH` 前缀；纯渲染 GPU 不作为显示就绪的依据。补丁位于
`patches/gdm/<版本>/`，Debian 和 Nyarch 构建安装对应版本的标准软件包。
修正复用 GDM 原有的 udev 事件与超时，不增加显示服务、固定延时
或桌面重启脚本。硬件运行时安装同时清理显示管理器对旧 `piano-display.service`
的专用依赖，保留其他用户配置。软件桌面截图只证明合成器渲染成功，内屏输出需
结合 DRM 活动平面和摄像头实拍确认。

Debian KDE 的 SDDM 在自动登录前通过 `udevadm wait` 等待内屏控制器的 DRM
设备，避免 KWin 在 `simpledrm` 被接管时退出。使用 udev 的平台设备路径，
不依赖 `card` 的枚举顺序；设备就绪后立即开始登录，超过十秒仍尝试正常启动。

锁屏重新认证也使用 GDM 原有的 PAM 和 D-Bus 通道。客户端取消时关闭认证会话，
不再向取消方发送 `Reset`，避免 GNOME Shell 51 再次取消、重建认证的循环。
请求被替换或客户端断开时，先关闭认证会话、停止 PAM 子进程，再释放对象。
认证进程仍持有会话引用，仅释放请求本身不足以结束它们。
该补丁由 Debian 和 Nyarch 的 GDM 系列共用，不改变密码校验、锁屏设置
或文件描述符上限。
