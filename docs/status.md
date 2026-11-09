# 项目状态

本页按功能列出当前状态，包含维护者与贡献者完成的适配。首页的表格是它的简化版；各项的技术细节在 `docs/devel/` 的对应页面中。

## 测试设备

维护者使用小米平板 8 Pro（`piano`，型号 `25091RP04C`，SM8750P，16 GB 内存），原厂系统为 Android 17／HyperOS 4（`OS4.0.5.0.XPYCNXM`），Bootloader 已解锁。Linux 按原厂引导程序传来的标识选择 BOE／CSOT 面板，见[面板选择](devel/hardware/panel-selection.md)。合体 BOOT 支持国行和国际版 Piano。

这台机器划出了 `sunuefi_esp`（512 MiB FAT32，4 KiB 扇区，与 UFS 逻辑扇区一致）和 `sunuefi_root`（256 GiB ext4），已运行 Debian 13 / GNOME、Nyarch / GNOME 和 SteamOS 0.6.0。默认内核为 `7.3.3-piano-sunuefi-cachy`，从合体 BOOT 经 UEFI 启动，支持来回切换 Android 与 Linux。四个发行版共用内核、BSP 和原厂 DSP 固件；root 镜像首次启动会自动扩展到分区容量。

## 启动与安装

| 项目 | 状态 |
| --- | --- |
| 合体 BOOT：Android 直通 | 可用。普通开机进入原厂 Android |
| 合体 BOOT：保存 Linux 路线并重启 | 可用。路线在 Android 侧保存，重启后经 UEFI 进入内部磁盘 Linux |
| 标准 EFI 启动 | systemd-boot 直接加载内核 EFI stub、initramfs 和 DTB，内存布局由同次启动的原厂设备树和 SMEM 提供；旧 ESP 保留兼容入口 |
| Linux 返回 Android | 可用。`piano-next-boot android --reboot`，见[Linux 下次启动入口](devel/boot/linux-next-boot.md) |
| 原厂 Recovery | 保留。`reboot recovery` 进入 Mi Recovery。持久 Linux 偏好与 Recovery 同时存在时的行为还没有在设备上试过 |
| 独立 Recovery 安装 | 不可用。ABL 对“Recovery 自带内核”的分支不加载 vendor_boot 和 pvmfw，启动停在 Fastboot；合体 BOOT 走普通启动路径，绕开了这个问题。安装器拒绝 `--recovery` |
| 启动路线管理 | 模块保存 Android／UEFI 路线，普通开机沿用；前置选择页显示约三秒，音量键可临时改选，电源键确认。临时选择不改保存的路线 |
| 合体 BOOT 在线安装 | 模块从当前 BOOT 提取 Android 内核并安装或更新同一份 UEFI 核心；HyperOS 更新后可点击“重新安装引导”。已有核心升级已在设备上完成，所有 OTA 场景尚未覆盖。见[原生 BOOT 重打包](devel/boot/combined-boot.md) |
| 一键安装器 | 更新已有的 `sunuefi_esp` 与 `sunuefi_root`；UEFI 只读原厂蓝牙地址并通过标准设备树属性传给 Linux，不依赖安装阶段配置。新平板首次分区返回 `NEW_INSTALL_NOT_READY` |
| Android Root 模块 | 提供可安装 ZIP，包含启动选择、引导检查、重新安装和独立存储管理。SukiSU 安装和 WebUI 可用；删除后的空间归还已通过早期离线 F2FS 扩容完成。新设备首次分区、root 容量调整仍需实机验证，见[引导助手](devel/boot/android-module.md) |

## UEFI

| 项目 | 状态 |
| --- | --- |
| 存储与保护 | 可用。探测前恢复 UFS 设备就绪状态，读取 GPT、BlockIO 和 ESP 文件；原厂关键分区由代码强制只读 |
| 交接与退出 | 启动 Linux 前清理 USB、UFS、时钟与 SMMU，无法确认安全时拒绝交接 |
| 显示 | 合体 BOOT 进入 UEFI 后可显示菜单；延后原厂 ClockDxe 的 CESTA/media 和 DCD 启动初始化，保留继承的显示状态。见[显示定位](devel/boot/uefi-display.md) |
| 菜单 / Setup / Shell | SimpleInit 菜单、TianoCore 中文设置页面与四倍字号可用；音量键移动、电源键确认、双音量键返回可用，见[设置页面](devel/boot/uefi-console.md)；Shell 尚未完成实机验证 |
| Y700 四代触屏 | NT36536 的单指菜单点击、滑动可用，见[驱动说明](devel/hardware/y700-touch.md) |
| Piano 触屏、键盘、USB Host | 产品已包含输入后端；触屏大帧读取仍有校验错误，键盘导航和 USB Host 尚未完成实机检查。菜单继续使用音量键与电源键 |
| UEFI Fastboot | 在菜单、固件设置和 Shell 中常驻，正常 BOOT 进入时可连接；提供日志、截图、只读 `fetch`、`stage`（≤64 MiB）、`oem boot-stable` 和重启。拒绝 `flash`/`erase`；完整命令见[UEFI Fastboot](user/fastboot.md) |
| EFI 变量 | 普通非易失变量保存在 `persist/sunuefi/` 的双副本文件中；退出 Boot Services 后不再写 UFS，见[变量保存](devel/boot/uefi-console.md#persist-中的-uefi-变量) |
| 任意 `.img` / `.efi` 启动 | 没有完成，目前只启动 ESP 中的 Linux |

## Linux 与硬件

| 项目 | 状态 | 已知问题 |
| --- | --- | --- |
| 显示 | 3200×2136，默认 120 Hz，低分辨率缩放、144 Hz、KDE HDR／广色域切换、十位输出、夜灯、标准 Gamma／Degamma及熄屏恢复 | 12-bit／FRC 和色深选择尚未提供，见[显示位深](devel/hardware/display-depth.md) |
| GPU | Adreno 加速的 GNOME | 部分 GTK 窗口连续缩放约 30 次/秒画面提交，需继续区分客户端处理和帧回调等待；见[窗口缩放](devel/hardware/window-resize.md) |
| 触屏 | 点按、拖动、长按、手势，约 144 Hz | 360 Hz 未实现；依赖主机端触点算法和 uinput |
| 键盘 / 触控板 | 打字、触控板、背光和睡眠恢复可用，背光走 UPower / GNOME | — |
| 触控笔 | 定位、压感绘画、144 Hz 笔输入、轻捏与震动；磁吸识别、笔电量、充满停电以及跨系统自动重新配对 | 完整掌压、笔身滑动及应用快捷手势仍需完善，见[笔协议](devel/hardware/pen-protocol.md) |
| Wi-Fi | 可用 | 新系统需自行保存网络连接 |
| 蓝牙 | 可用，使用 p81 板级 NVM，已修复连接距离过短；启动时自动读取本机原厂地址 | 原厂地址缺失时提供稳定硬件标识派生方案，见[蓝牙配置](devel/hardware/bluetooth.md) |
| Linux USB 3 | 标准 Type-C 方向和角色切换、5 Gbit/s USB 调试网络 | 首次连接时序继续核对，见[USB Type-C](devel/linux/kernel.md#usb-type-c) |
| 外接存储 | U 盘、移动硬盘、读卡器可用，FAT32／exFAT／NTFS 读写 | — |
| 扬声器 | 四路播放、音量调节和睡眠恢复，沿用原厂功放增益 235 | — |
| 麦克风 | 录音、原生单声道 ALSA / PipeWire 输入及睡眠恢复，硬件增益 +40 dB | — |
| 相机 | 前后预览、录像、自动曝光／对焦／白平衡及首帧恢复，CAMSS/TFE 硬件 ISP | 实现见[相机与闪光灯](devel/hardware/camera-flash.md)，手动接口见[相机控制](devel/hardware/camera-controls.md) |
| 相机隐私灯 | 摄像头工作时实体绿色指示灯亮起，随视频流和前后摄像头切换启停 | — |
| 闪光灯 | 两路标准 LED 已注册，限值已读回 | 实际发光和曝光同步未观察 |
| 传感器 | 自动旋转、光线、距离、罗盘及标准 SensorProxy 接口 | 实现见[传感器](devel/hardware/sensors.md) |
| 自动亮度 | 桌面联动可用，GNOME 策略沿用原厂 lux 阈值与延迟，传感器数据不做修改 | 见[自动亮度](devel/hardware/auto-brightness.md) |
| 夜灯 / 色温 | GNOME 夜灯可用，自动生成内置屏幕的颜色配置 | 通用 sRGB 配置，不是原厂校准 |
| 电源 | 双电池、PD 与小米快充、拓展坞充电和 OTG、UPower、合盖恢复、挂起及 RTC／电源键唤醒；2 分钟熄屏，电池 5 分钟、外接电源 10 分钟自动挂起 | 不启用磁盘休眠。见[电源键](devel/hardware/power-button.md) |
| GNOME 电源模式 | 节能／平衡／性能三档可用，默认平衡，由 TuneD 与 tuned-ppd 提供 | 档位调整 CPU／GPU 上限，不改变待机功耗，见[电源模式](devel/hardware/power-profiles.md) |
| 视频编解码 | ✅ | Iris 硬件加速与 V4L2 编解码可用，提供适配的 FFmpeg 软件包 |
| SteamOS / Proton / FEX | ✅ | 游戏模式和桌面模式正常运行，ARM64 Proton 与 FEX 支持良好（个别游戏需自身参数，与项目本身无关） |
| 指纹 | Linux 独立录入、保存与认证，标准 libfprint／fprintd 接口 | 不读取 Android 的指纹模板，不提供跨用户识别；见[指纹实现](devel/hardware/led-ir-fingerprint.md#侧指纹) |
| 红外 / RGB 灯 | RGB 标准 LED 接口、红外标准 LIRC 驱动和原厂波形量化已接入 | 红外光学输出未测量 |
| 默认桌面 | 中文界面、拼音输入法、Chromium 浏览器，GNOME 动画开启 | — |
| Windows / WinPE | 目标 | 未启动 |

### 其它发行版

CI 提供 Debian / GNOME、Debian / KDE、Nyarch / GNOME 和 SteamOS，生成模块、ESP、各发行版 root 稀疏镜像、临时启动 UEFI 及日志。Debian / GNOME、Nyarch / GNOME 和 SteamOS 已在设备上运行；Debian / KDE 的完整实机体验仍需验证。各发行版使用同一套内核和 BSP。

120 Hz 的默认选择由内核面板驱动补丁提供，并在 `config/release.json` 中记录；不是登录桌面后才切换刷新率。BOE／CSOT 仍按设备标识选择，不固定面板供应商，也不移除其他显示模式。挂起通过标准 systemd／logind 接口启用，GNOME 按上述空闲时间触发。休眠、混合睡眠和先挂起后休眠保持关闭。
