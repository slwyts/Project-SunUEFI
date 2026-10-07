# 项目状态

更新日期：2026-10-08。本页记录已保存的源码、构建和设备观察，不表示今天重新验收了每项硬件。项目仍为 **`INCOMPLETE_NOT_RELEASE`**；当前构建与已运行镜像必须分开看。

## 验证范围

设备记录来自小米平板 8 Pro `piano`，型号 `25091RP04C`，SM8750P，16 GB 内存，原厂标识为 CSOT 面板，Android 16 / `OS3.0.309.0.WPYCNXM`，已解锁 Bootloader。其他面板、容量、地区 ROM 和版本未由这些记录证明兼容。

研究设备已按所有者授权划分 `sunuefi_esp`（512 MiB FAT32）和 `sunuefi_root`（63.5 GiB ext4），并安装 Debian 13/GNOME。该部署不是公开的一键安装流程。Recovery 实验后已恢复原厂 Recovery，并验证普通 Android 磁盘启动；不能将它描述为 UEFI 已持久安装。

## 最近实机结果（2026-10-08）

合体 BOOT 已普通重启进入新构建的 Linux `7.2.9-piano-gnome-gc24355c2de05`，源码 `c24355c2de05ccfda69abc113c6b01da6a983919`。这轮重新部署了 ESP 与 `PIANOROOT`，不使用完整 RAM 根系统。UEFI 根据原厂 ABL 的 CSOT 标识修改交接 DTB，实机读回 CSOT；其他面板尚未实测。

ESP 打包已改为 4 KiB FAT32 扇区，与本机 UFS 逻辑扇区一致。修正后 Linux 正常挂载 `/boot/efi`，再次普通 BOOT 重启进入同一磁盘系统。CPU 名称已显示 Qualcomm Snapdragon 8 Elite Mobile Platform (SM8750)。触屏、键盘、ADSP、音频和无线服务在新系统中启动成功；旧系统的人工功能观察见下表，新构建不能继承未复测的全部体验结论。

相机／视频的额外寄存器诊断已移到原生驱动绑定之后，保留原生 IOMMU、模块与绑定的实际失败处理。OV32D40、S5KJN1 和 Iris 已绑定；前后两路各取得约28个真实 ISP 帧。启动时黑帧与曝光收敛已观察，颜色矩阵 ioctl 写回只读数组的错误已修正并重新编译；画质、应用使用和性能仍在验证。

SSC 已安装固定来源的 FastRPC/libssc/iio-sensor-proxy 包，QMI 通信和 registry 可访问，但物理传感器 UID 尚未返回。已从本机原厂 Vendor 只读提取缺失的 `sns_reg_config`，补入标准导入补丁及启动模块依赖，正在重启验证。不能据服务 active 宣称旋转、光感等已正常。

本轮麦克风用同一增益比较 DMIC1/DMIC2，尚未获得足以证明质量改善的结果，默认路由未改。HDR/12-bit、触控笔完整功能、闪光灯、合盖与休眠仍未完成实测。当前产物仍为开发构建。

以下为此前候选与定位记录，其“最新”字段只指当时的状态。

## 先前候选与设备结果

| 项目 | 身份与范围 |
| --- | --- |
| 唯一产品路径 | `artifacts/product/PianoUEFI-product.img`；此路径是可被新构建更新的候选位置 |
| 最新已编译候选 | 28,925,952 字节；SHA256 `6f5635ac32035b7068eb3fec68e05b609eb36b77bd9ef42994bc132637a19ee2`；build ID `d2a6d24e-5fb9-4829-b814-ab0a84a8c6ed` |
| 最新候选变化 | 删除历史 GPT 运行时／构建依赖，动态发现 ESP，缺少 Setup 协议时返回菜单；合体 BOOT 的 Android 路径通过，显式 UEFI 仍在 USB 初始化超时 |
| 最近完整设备会话 | 第 114 次；产品镜像 SHA256 `33e9d6babcc74cd264b9cd14a42ef37cf991f32b29d094d2130dd3c6591022ad`，与最新候选不同 |
| 第 114 次 UEFI | 标准 Fastboot 枚举、256 KiB 日志读取、从真实 ESP 请求 Stable 启动成功；显示 AHB 保持启用，但物理屏幕仍白 |
| 第 114 次 Linux | `7.2.6-piano-gnome-00069-gefe5734c2451`，源码 `efe5734c24510c3c194f765511b49be9f13b4aa0`，匹配模块及小型磁盘 initramfs，真实 ext4 根系统进入 GNOME |

封存镜像、原始设备记录在本地 `artifacts/tests/`、`private/analysis/`。本页提供可公开的身份摘要；原始材料没有随源码发布。主机构建 manifest 的 `device_boot_performed=false` 不会被后续设备记录悄悄改成通过，验收要引用单独会话。

## 本轮构建与安装进展

- UEFI 已从 `uefi/` 正式来源、固定 submodule 和最小 vendor 输入重建成功；104项公开主机检查通过，OS加载副本的5项检查通过。
- 新 LABEL 内核 `7.2.6-piano-gnome-00069-g8524f36a3489` 已完整编译，1637个模块配套，源码树等同原 Kernel69；未换入当前设备。
- 根分区已从旧名 `sunuefi_linux` 改为 `sunuefi_root`，UUID、边界、容量和ext4数据不变。设备正常回Android，原Kernel69的ESP bootstrap已按新名字同步并读回。
- Recovery 的 v3 与仅头格式改变的 v4 对照都回到原厂Fastboot，没有确认执行到UEFI。原厂Recovery已恢复并读回，普通Recovery菜单→Android路径成功；未修改全局vbmeta，未进行常规RAM启动。
- BSP配置层已生成实际 `.deb`，多发行版/桌面适配器有真实后端和输入检查；完整rootfs/ESP端到端构建、其他目标二进制层和GitHub实际CI尚未运行。
- 安装器可计划并更新已存在的两专用分区；首次缩容/写GPT执行尚未开放。

## UEFI 功能

| 功能 | 设备记录 | 未完成 / 限制 |
| --- | --- | --- |
| GOP / 中文 UI | 旧版 SimpleInit 可见且可操作；第 97 次重放同一旧镜像仍正常；GOP 像素读写与截图有证据 | 当前集成固件存在白屏；截图正确不能证明物理扫描输出，最新兼容候选未验收 |
| 实体按键 | 第 23–24 次音量移动、短按并松开电源触发确认，进入工具页 | 当前集成显示/完整快捷键需联合验收 |
| USB Device / Fastboot | SuperSpeed、日志、截图、只读分区 fetch、reboot 和部分退出路径有设备记录；第 114 次产品枚举与日志成功 | 所有 UI 的持续服务、重连、全部退出组合仍需验收；一般 flash/erase 不开放 |
| UFS / BlockIO / SFS | 六个 LUN、GPT、139 个只读 BlockIO 句柄、七个 SFS 卷及 Shell 枚举；真实 ESP Linux 文件读取 | 一般产品写入未开放；早期专用区域写入/读回/恢复实验不是通用写权限 |
| DMA / SMMU | UFS/USB 的受控传输与上下文退出有实测 | QUP/GPI 真实传输、高 DDR 与 NC common-buffer 等未全部验收 |
| SimpleInit / Setup / Shell | 同一核心和共同服务源码已集成；旧中文 SimpleInit 工具页实测 | Setup/Shell/当前菜单物理显示及全部后台场景未联合验收 |
| 触屏 / 官方键盘触控板 | 软件输入、协议和部分传输准备 | 真实 UEFI 触点和 pogo 报告后端未验收；不借用 Linux 成功结论 |
| 调试输出 | RAM 日志、重启后回收及 USB 日志快照有实测 | RAM SerialPortLib 不是物理 UART，实时 UART 未完成 |
| 下载与通用内存启动 | 当前 Fastboot 下载上限 64 MiB；受限 Linux 启动后端已接入 | 1 GiB 为目标；通用 `.img` / `.efi` 和高内存加载未完成 |
| USB Host / 持久变量 | 有接口、源码和主机验证 | 外设/角色切换未验收；EFI 变量仍在 RAM，产品 NV 卷未配置 |
| 启动入口 | `fastboot boot` 临时启动已有记录 | 合体 BOOT 的 Android 普通入口与原厂 Mi Recovery 返回已验证；显式 UEFI 请求仍需完成存储与消费流程 |

## Linux 功能

| 功能 | 设备 / 用户确认 | 未完成 / 限制 |
| --- | --- | --- |
| 桌面与显示 | tty 后进入 GNOME，Adreno 信息及亮度调整可见 | 启动慢；窗口缩放的提交速率与延迟待定位，不能承诺全桌面 144 fps |
| 手指触控 | 点击、拖动窗口、长按菜单有用户确认；THP helper 稳定输入约 144 Hz 的记录 | 360 Hz 未实现；firmware getter 的扫描/报告字段不是所有场景的输入率；目前依赖主机触点算法与 uinput |
| 官方键盘 / 触控板 | 输入、指针与触控板操作有用户确认；背光实机亮起，标准 UPower / GNOME 接口调光成功，用户确认 Keyboard 控制出现 | 完整按键、合盖和 suspend/resume 回归未完成；背光启动顺序修复尚未经过下一次冷启动验证 |
| Focus Pen Pro | 蓝牙连接并经 UHID 注册输入设备 | 当前注册设备没有笔坐标/压感字段；THP 笔输入、轻捏、震动、无线充电与隔空功能未完成验证 |
| Wi-Fi | 用户确认联网，系统包下载有实际记录 | 更多 AP、漫游、恢复场景未验收 |
| 蓝牙 | 控制器加载、发现附近设备 | 配对、音频与更多连接场景未验收 |
| 扬声器 | GNOME 左右测试音由用户确认正常 | 不能据此证明四扬声器、所有路由和休眠恢复完成 |
| 麦克风 | DMIC1 录音回放可辨认手机音乐 | 噪音较大，GNOME 输入音量条异常；质量未验收 |
| 电源 / 充电 / 休眠 | 实体音量、电源及部分状态有观察 | 关机曾回到 Android，最终 poweroff/PMIC 唤醒原因未定位；双电池／基础 PD 充电与 UPower 已验证；高压快充、合盖、休眠和完整充电周期未验收 |
| 其他硬件 | 部分驱动已构建 / 注册 | 摄像头、完整视频链、笔与其他未列功能不能视为通过 |

当前 Linux 使用板级适配内核、DTB、固件和发行版集成。Ubuntu/Arch 等替换根系统还需要匹配模块、Mesa、UCM、输入 helper 与平台配置，不能宣称任意发行版直接完整可用。下一步应把硬件依赖和 probe 时序尽可能归入内核/DTB，保留有明确作用的标准用户态集成。

## 白屏与慢启动是两个问题

原生基础驱动加入后的显示回归有时间窗口和时钟变化证据，但确切触发点尚未证明。第 114 次保住显示 AHB 后仍白；Linux 后续原生 DPU/双 DSI 接管才显示桌面。[显示回归记录](piano-display-regression-window.md)保留详细对照。

第 114 次 Linux 从 ext4 根分区启动，`systemd-analyze` 为 58.748 秒 kernel + 71.725 秒 userspace = 130.474 秒，不包含 UEFI 时间。不是完整 RAM 根系统解压的测量。设备出现、显示与调试服务均有较长阶段；等待上限和 `blame` 列表不能直接相加当成关键路径，尚无经验证的提速结论。

## 记录规则

更新状态须写明环境、源码/镜像身份、测试方法、实际观察及限制。驱动注册、编译、主机夹具通过、USB `OKAY` 与最终功能成功分别记录。新构建不能继承旧镜像的整机验收；原生 ARM64 交接、标准 EFI-stub、RAM 根和磁盘根也是不同路径。

## 前置选择器与存储检查修复

2026-10-07：修复 MMU 关闭时 DTB 64位属性未对齐读取后，正式源码构建的合体 BOOT 已正常进入 Android；标准 `reboot recovery` 仍进入 Mi Recovery，并返回同一产品的 Android。具体镜像与读回记录见[独立请求](devel/reboot-request.md)。

显式 CRC 请求已进行实机观察，但尚未完成 UEFI 请求自动消费／完整入口验收。旧产品存储后端的历史 GPT 比对及生产输入依赖已从源码删除；新构建和设备结果将单独记录。

本轮清理后的显式入口日志已经越过旧存储检查，返回 `EFI_NOT_FOUND` 表示未配置可写容器并继续初始化；停点为 USB `Time out`，不属于 Linux 桌面启动。已恢复无请求新产品包并确认 Android 启动和读回。ESP 的历史 UUID/LBA/大小绑定也已删除，Setup 的明确缺协议状态可以返回菜单；这两项主机测试通过，联合设备体验仍待验证。
