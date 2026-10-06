# Project SunUEFI — 小米平板 8 Pro (`piano`)

Project SunUEFI 是为小米平板 8 Pro（代号 `piano`，搭载骁龙 8 至尊版 / SM8750 芯片）开发的开源 UEFI 固件与跨系统引导项目。

项目的长期目标是在该设备上实现 **UEFI 固件**、**主线 Linux** 与 **Windows on ARM** 的稳定运行。

**目前仍是开发候选（`INCOMPLETE_NOT_RELEASE`）。最新构建尚未实机测试，不能当作稳定安装版。**

---

## 项目目标

1. **统一的 UEFI 固件**：构建单一产物 `PianoUEFI-product.img`，提供图形启动菜单、标准 ACPI 表与安全的运行环境。
2. **主线 Linux 支持**：摆脱原厂下游内核包袱，推进 SM8750 上游主线驱动，完善触控输入、键盘皮套、音频与日常桌面体验。
3. **Windows on ARM 支持**：补齐板级 ACPI 与核心驱动链，逐步推进 WinPE 引导与 Windows ARM64 桌面运行。

---

## 当前进展

目前已打通从 UEFI 到独立磁盘分区的启动链路，并成功进入 GNOME 图形桌面：

* **已可用**：
  * 支持通过电脑以 `fastboot boot` 临时加载固件运行（不改动任何原厂启动分区）。
  * 从平板内部的专用 ESP 和 ext4 分区引导 Linux。
  * 正常进入 **GNOME 桌面**，Adreno GPU 加速、**约 144 Hz 的触控输入**、**官方键盘皮套与触控板**、**Wi-Fi 联网**及扬声器发声均可正常工作。
* **已知关键问题**：
  * **UEFI 启动阶段物理白屏**：部分记录中显存内容正常，屏幕却仍白。时钟初始化是调查方向，具体原因还未确认；Linux 后续接管后已能显示桌面。
  * **启动耗时较长**：最近一次磁盘 Linux 启动约 130 秒，具体瓶颈仍在定位。
  * **持久启动未完成**：Recovery 实验未成功，失败阶段尚未定位，原厂 Recovery 已恢复。当前使用临时启动路径。
  * 麦克风底噪、待机休眠等细节功能仍在调试中。

---

## 硬件支持状态

> 基准机型：小米平板 8 Pro (`piano`) 16GB，BOE 屏幕，已解锁 Bootloader。下表的输入、无线、音频和电源均指 Linux；UEFI 的触屏和键盘后端尚未完成。

| 组件 / 功能 | 状态 | 说明 |
| :--- | :---: | :--- |
| **UEFI 固件引导** | ⚠️ 开发中 | 支持 `fastboot boot` 临时启动；UEFI 阶段屏幕物理白屏（修复中） |
| **Linux 图形桌面** | ✅ 正常 | 正常进入 GNOME 桌面，Adreno GPU 加速可用 |
| **屏幕多点触控** | ✅ 正常 | 点按、拖动、长按正常，稳定输入报告约 144 Hz，不能等同于传感器最高采样率 |
| **官方键盘 / 触控板** | ✅ 正常 | 磁吸 Pogo-pin 键盘打字与触控板指针操作正常 |
| **无线网络 (Wi-Fi)** | ✅ 正常 | PCIe 接口网卡正常驱动，已确认扫描热点和联网 |
| **蓝牙 (Bluetooth)** | ⚠️ 基础 | 固件加载正常，可扫描周边设备，配对与音频完善中 |
| **声音输出 (扬声器)** | ✅ 正常 | 左右立体声扬声器正常发声 |
| **声音输入 (麦克风)** | ⚠️ 有底噪 | 能录入声音，底噪偏大，电平调节待优化 |
| **电池与充电** | ⚠️ 基础 | 可读取基础状态，高级电源管理与待机尚未完善 |
| **Windows on ARM** | 🔄 规划中 | 板级 ACPI 构建中，准备推进 WinPE 早期引导 |

---

## 设计原则

* **单一固件核心**：所有功能收敛到单一镜像 `PianoUEFI-product.img`，避免分散维护碎片化的测试版本。
* **原厂数据安全**：底层对原厂 Android 分区（系统、基带、凭据）强制写保护，正常退出路径冷重启回 Android；卡住时可能需要手动恢复。
* **常驻 Fastboot 调试端**：UEFI 阶段后台暴露 USB 设备（`SunUEFI-piano`），白屏时也可提取运行日志、显存截图或请求安全重启。

---

## 快速入口

* **使用与体验**：
  * [快速体验指南](docs/user/getting-started.md)：如何用电脑临时加载固件并进入 Linux
  * [安全返回 Android](docs/user/recovery.md)：意外状况下的自救与退回原厂系统
  * [已知问题与排查](docs/user/known-issues.md)：白屏、启动耗时与注意事项
  * [Fastboot 调试命令速查](docs/user/fastboot.md)：日志抓取与显存截图
* **开发与构建**：
  * [编译与构建手册](docs/devel/building.md)：从源码构建 `PianoUEFI-product.img`
  * [源码目录地图](docs/devel/repository-map.md)：代码与工具链分布
  * [全量硬件状态记录](docs/status.md)：技术细节与底层验证
  * [贡献指南](CONTRIBUTING.md) 与 [AI 协作规范](AGENTS.md)

---

## 协议与来源

* UEFI 固件移植基于 [TianoCore EDK2](https://github.com/tianocore/edk2) 与 [Project Silicium (Mu-Silicium)](https://github.com/Project-Silicium/Mu-Silicium)，相关源码保留各自许可；本仓库包含 BSD、GPL、LGPL 等不同声明，具体范围见下方说明。
* 启动菜单前端基于 [simple-init](https://github.com/BigfootACA/simple-init)。
* 完整第三方来源与许可证见 [THIRD_PARTY.md](THIRD_PARTY.md) 与 [LICENSE.md](LICENSE.md)。
