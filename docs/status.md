# 项目状态

更新日期：2026-10-09。本页按功能列出当前状态。首页的表格是它的简化版；各项的技术细节在 `docs/devel/` 的对应页面中。

## 测试设备

所有结果来自同一台小米平板 8 Pro（`piano`，型号 `25091RP04C`，SM8750P，16 GB 内存），原厂 Android 16（`OS3.0.309.0.WPYCNXM`），Bootloader 已解锁，当前运行使用 CSOT 面板配置（由原厂引导程序传来的标识选择，不等同于对物理供应商的识别）。Linux 对面板的选择见[面板选择](devel/panel-selection.md)；其他面板、内存容量和 ROM 版本没有测试过。

这台机器划出了 `sunuefi_esp`（512 MiB FAT32，4 KiB 扇区，与 UFS 逻辑扇区一致）和 `sunuefi_root`（63.5 GiB ext4），安装了 Debian 13 / GNOME。当前 Linux 内核为 `7.2.9-piano-gnome-gc24355c2de05`，从合体 BOOT 经 UEFI 启动，已实际来回切换 Android 与 Linux。

## 启动与安装

| 项目 | 状态 |
| --- | --- |
| 合体 BOOT：Android 直通 | 可用。普通开机进入原厂 Android |
| 合体 BOOT：保存 Linux 路线并重启 | 可用。路线在 Android 侧保存，重启后经 UEFI 进入内部磁盘 Linux |
| Linux 返回 Android | 可用。`piano-next-boot android --reboot`，见[Linux 下次启动入口](devel/linux-next-boot.md) |
| 原厂 Recovery | 保留。`reboot recovery` 进入 Mi Recovery。持久 Linux 偏好与 Recovery 同时存在时的行为还没有在设备上试过 |
| 独立 Recovery 安装 | 不可用。ABL 对“Recovery 自带内核”的分支不加载 vendor_boot 和 pvmfw，启动停在 Fastboot；合体 BOOT 走普通启动路径，绕开了这个问题。安装器拒绝 `--recovery` |
| 启动路线管理 | 已有：保存、读取、`piano-boot-request`、`piano-next-boot`。未做：菜单选择自动记录、一次性请求自动清除、与 OTA 联动 |
| 合体 BOOT 在线安装 | 原生重打包工具已能无损还原真实原厂 BOOT，在线写入、OTA 后重打包尚未做进安装器。见[原生 BOOT 重打包](devel/android-boot-repack.md) |
| 一键安装器 | 只更新已有的 `sunuefi_esp` 与 `sunuefi_root`，并写入本机蓝牙地址。新平板首次分区返回 `NEW_INSTALL_NOT_READY` |
| Android Root 模块 | `./build.sh module --inspect` 只列出缺项，没有可安装 ZIP。WebUI 为预览 |

## UEFI

| 项目 | 状态 |
| --- | --- |
| 存储与保护 | 能读取 UFS 的 GPT、BlockIO 和 ESP 文件；原厂关键分区由代码强制只读 |
| 交接与退出 | 启动 Linux 前清理 USB、UFS、时钟与 SMMU，无法确认安全时拒绝交接 |
| 显示 | GOP 帧缓冲内容正确（截图可证），但集成版本的物理屏幕白屏，原因没有确定。进入 Linux 后由 Linux 驱动重新显示 |
| 菜单 / Setup / Shell | 旧版 SimpleInit 中文菜单和实体按键可用；当前集成版本受白屏影响 |
| 触屏、键盘、USB Host | UEFI 阶段没有完成，不使用 Linux 的结果代替 |
| UEFI Fastboot | 已实现：`oem status`、`oem ramlog`、`oem screenshot`、只读 `fetch`、`stage`（≤64 MiB）、`oem boot-stable`、`reboot`。拒绝 `flash`/`erase`。冷启动合体 BOOT 进入时 USB 初始化曾超时；完整命令见[UEFI Fastboot](user/fastboot.md) |
| EFI 变量 | 保存在内存，没有持久存储 |
| 任意 `.img` / `.efi` 启动 | 没有完成，目前只启动 ESP 中的 Linux |

## Linux 与硬件

| 项目 | 状态 | 已知问题 |
| --- | --- | --- |
| 显示 | 3200×2136 144 Hz，手动亮度 | 60 / 120 Hz 切换、关屏和合盖后恢复可能黑屏，双 DSI PHY 重同步的修正已提交，仍待验证；HDR、12-bit、VRR 未完成 |
| GPU | Adreno 加速的 GNOME | 窗口缩放时曾有约 30 次/秒的提交，原因未定位 |
| 触屏 | 点按、拖动、长按、手势，约 144 Hz | 360 Hz 未实现；依赖主机端触点算法和 uinput |
| 键盘 / 触控板 | 可用，键盘背光走 UPower / GNOME | 特殊键和睡眠恢复未完成 |
| 触控笔 | 蓝牙连接，注册了没有坐标字段的输入设备 | 坐标、压感、悬停、按键、震动、无线充电未完成，见[笔协议](devel/piano-pen-protocol.md) |
| Wi-Fi | 可用 | 新系统需自行保存网络连接 |
| 蓝牙 | 控制器启动即出现，扫描、配对和连接可用 | 每台平板需要本机地址（通过 DT 写入，见[蓝牙地址](devel/piano-bluetooth.md)）；蓝牙耳机播放可用，其他音频与恢复场景继续完善 |
| 扬声器 | 四路都能发声，沿用原厂功放增益 235 | 睡眠恢复未测 |
| 麦克风 | 能录音 | 单声道混合造成衰减，真正的单声道修正已编译、未部署；噪声与桌面电平待调 |
| 相机 | 前后路预览、录像，CAMSS/TFE 硬件 ISP | 首帧绿色区域、启动偏暗、曝光收敛慢、对焦与画质差距；启动曝光与帧完整性修正已构建、未在设备上验证；与原厂 CamX/CHI 不同，3A 为软件实现。见[相机与闪光灯](devel/piano-camera-flash.md) |
| 闪光灯 | 两路标准 LED 已注册，限值已读回 | 实际发光和曝光同步未观察 |
| 传感器 | SSC / FastRPC / iio-sensor-proxy 已装，多种数据可读 | 桌面联动和单位未全部完成，见[传感器](devel/piano-sensors.md) |
| 自动亮度 | GNOME 策略包沿用原厂 lux 阈值与延迟，传感器数据不做修改 | 依赖传感器联动完成度，见[自动亮度](devel/piano-auto-brightness.md) |
| 夜灯 / 色温 | GNOME 夜灯可用，自动生成内置屏幕的颜色配置 | 通用 sRGB 配置，不是原厂校准 |
| 电源 | 双电池、基础 PD 充电、UPower | 关机偶尔回到 Android；合盖和电源键只处理背光；高压快充和睡眠未验证，见[电源键](devel/piano-power-button.md) |
| 指纹 | 原厂 QSEECom 通路已定位 | Linux 无后端 |
| 红外 / RGB 灯 | RGB 灯接口出现，红外 LIRC 候选已准备 | 红外发射未验证 |
| Chromium、中文输入法、默认动画 | Chromium 已安装并替换 Firefox；拼音输入源、默认中文与动画配置已写入 | 全新镜像首次启动验证正在进行 |
| Windows / WinPE | 目标 | 未启动 |

### 启动时间

`systemd-analyze` 测得的内核与用户空间时间合计约一到两分钟，不含 UEFI 阶段，数字随构建变化，不是固定指标。具体测量方法和对比见[启动时间](devel/piano-boot-time.md)。

### 其他发行版

`linux/bsp/`、`linux/desktops/` 和 `linux/rootfs/` 为 Debian、Ubuntu、Deepin、Arch 以及 GNOME、KDE 提供配置，但目前只有 Debian / GNOME 完整构建并在设备上运行过。

## 文档记录约定

状态更新写明设备、软件版本和实际观察。驱动加载、编译通过、USB 命令成功和功能可用是不同的结论，分别记录。新构建不继承旧构建的设备结果。历史实验编号与旧结论见[历史索引](archive/README.md)。
