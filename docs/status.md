# 项目状态

更新日期：2026-10-07。本页记录已保存的源码、构建和设备观察，不表示今天重新验收了每项硬件。项目仍为 **`INCOMPLETE_NOT_RELEASE`**；当前构建与已运行镜像必须分开看。

## 验证范围

设备记录来自小米平板 8 Pro `piano`，型号 `25091RP04C`，SM8750P，16 GB 内存，BOE 面板，Android 16 / `OS3.0.309.0.WPYCNXM`，已解锁 Bootloader。其他面板、容量、地区 ROM 和版本未由这些记录证明兼容。

研究设备已按所有者授权划分 `sunuefi_esp`（512 MiB FAT32）和 `sunuefi_linux`（63.5 GiB ext4），并安装 Debian 13/GNOME。该部署不是公开的一键安装流程。Recovery 实验后已恢复原厂 Recovery，并验证普通 Android 磁盘启动；不能将它描述为 UEFI 已持久安装。

## 当前候选与最近设备结果

| 项目 | 身份与范围 |
| --- | --- |
| 唯一产品路径 | `artifacts/product/PianoUEFI-product.img`；此路径是可被新构建更新的候选位置 |
| 最新已编译候选 | 28,930,048 字节；SHA256 `828a093a18654c5655726ece03e15b5979d7fb9292891f09ec33f0686cbe2740`；build ID `8f231317-c6c0-44b2-a058-bcd7d52a12b4` |
| 最新候选变化 | 保留原生 CESTA 的七个临时时钟引用，延后显示 CESTA/PLL 自动初始化；只改变已审查的兼容派生，**尚未实机验证** |
| 最近完整设备会话 | 第 114 次；产品镜像 SHA256 `33e9d6babcc74cd264b9cd14a42ef37cf991f32b29d094d2130dd3c6591022ad`，与最新候选不同 |
| 第 114 次 UEFI | 标准 Fastboot 枚举、256 KiB 日志读取、从真实 ESP 请求 Stable 启动成功；显示 AHB 保持启用，但物理屏幕仍白 |
| 第 114 次 Linux | `7.2.6-piano-gnome-00069-gefe5734c2451`，源码 `efe5734c24510c3c194f765511b49be9f13b4aa0`，匹配模块及小型磁盘 initramfs，真实 ext4 根系统进入 GNOME |

封存镜像、原始设备记录在本地 `artifacts/tests/`、`private/analysis/`。本页提供可公开的身份摘要；原始材料没有随源码发布。主机构建 manifest 的 `device_boot_performed=false` 不会被后续设备记录悄悄改成通过，验收要引用单独会话。

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
| 启动入口 | `fastboot boot` 临时启动已有记录 | boot/recovery 持久入口未验收；Recovery 失败停点尚未定位 |

## Linux 功能

| 功能 | 设备 / 用户确认 | 未完成 / 限制 |
| --- | --- | --- |
| 桌面与显示 | tty 后进入 GNOME，Adreno 信息及亮度调整可见 | 启动慢；窗口缩放的提交速率与延迟待定位，不能承诺全桌面 144 fps |
| 手指触控 | 点击、拖动窗口、长按菜单有用户确认；THP helper 稳定输入约 144 Hz 的记录 | 360 Hz 未实现；firmware getter 的扫描/报告字段不是所有场景的输入率；目前依赖主机触点算法与 uinput |
| 官方键盘 / 触控板 | 输入、指针与触控板操作有用户确认 | 完整按键、合盖和 suspend/resume 回归未完成 |
| Wi-Fi | 用户确认联网，系统包下载有实际记录 | 更多 AP、漫游、恢复场景未验收 |
| 蓝牙 | 控制器加载、发现附近设备 | 配对、音频与更多连接场景未验收 |
| 扬声器 | GNOME 左右测试音由用户确认正常 | 不能据此证明四扬声器、所有路由和休眠恢复完成 |
| 麦克风 | DMIC1 录音回放可辨认手机音乐 | 噪音较大，GNOME 输入音量条异常；质量未验收 |
| 电源 / 充电 / 休眠 | 实体音量、电源及部分状态有观察 | 关机曾回到 Android，最终 poweroff/PMIC 唤醒原因未定位；合盖、休眠和充电全过程未验收 |
| 其他硬件 | 部分驱动已构建 / 注册 | 摄像头、完整视频链、笔与其他未列功能不能视为通过 |

当前 Linux 使用板级适配内核、DTB、固件和发行版集成。Ubuntu/Arch 等替换根系统还需要匹配模块、Mesa、UCM、输入 helper 与平台配置，不能宣称任意发行版直接完整可用。下一步应把硬件依赖和 probe 时序尽可能归入内核/DTB，保留有明确作用的标准用户态集成。

## 白屏与慢启动是两个问题

原生基础驱动加入后的显示回归有时间窗口和时钟变化证据，但确切触发点尚未证明。第 114 次保住显示 AHB 后仍白；Linux 后续原生 DPU/双 DSI 接管才显示桌面。[显示回归记录](piano-display-regression-window.md)保留详细对照。

第 114 次 Linux 从 ext4 根分区启动，`systemd-analyze` 为 58.748 秒 kernel + 71.725 秒 userspace = 130.474 秒，不包含 UEFI 时间。不是完整 RAM 根系统解压的测量。设备出现、显示与调试服务均有较长阶段；等待上限和 `blame` 列表不能直接相加当成关键路径，尚无经验证的提速结论。

## 记录规则

更新状态须写明环境、源码/镜像身份、测试方法、实际观察及限制。驱动注册、编译、主机夹具通过、USB `OKAY` 与最终功能成功分别记录。新构建不能继承旧镜像的整机验收；原生 ARM64 交接、标准 EFI-stub、RAM 根和磁盘根也是不同路径。
