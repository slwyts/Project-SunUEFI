# Project SunUEFI — 小米平板 8 Pro (`piano`)

Project SunUEFI 是为小米平板 8 Pro（代号 `piano`，搭载骁龙 8 至尊版 / SM8750 芯片）开发的开源 UEFI 固件与跨系统引导项目。

项目的长期目标是在该设备上实现 **UEFI 固件**、**主线 Linux** 与 **Windows on ARM** 的稳定运行。

**目前仍是开发候选（`INCOMPLETE_NOT_RELEASE`）。部分启动链路已实机验证，完整发布包仍不能当作稳定安装版。**

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
  * **持久启动正在完善**：修复前置选择器的 DTB 对齐读取后，源码构建的合体 BOOT 已正常启动 Android，并保留标准 `reboot recovery` 的 Mi Recovery 行为。显式 UEFI 请求及自动消费仍在验证；独立 Recovery 镜像路径依然受 ABL 缺载依赖限制。详见[入口分析](docs/devel/recovery-entry.md)与[请求机制](docs/devel/reboot-request.md)。
  * 麦克风底噪、待机休眠等细节功能仍在调试中。

---

## 硬件支持状态

> 基准机型：小米平板 8 Pro (`piano`) 16GB，已解锁 Bootloader；原厂运行参数识别为 CSOT 面板。固件按当前设备的原厂面板标识选择 Linux 配置，见[面板选择](docs/devel/panel-selection.md)。下表的输入、无线、音频和电源均指 Linux；UEFI 的触屏和键盘后端尚未完成。

| 组件 / 功能 | 状态 | 说明 |
| :--- | :---: | :--- |
| **UEFI 固件引导** | ⚠️ 开发中 | 合体 BOOT 已普通重启进入磁盘 Linux；原厂 Recovery 保留，UEFI 物理白屏仍在修复 |
| **Linux 图形桌面** | ✅ 正常 | 正常进入 GNOME 桌面，Adreno GPU 加速可用 |
| **屏幕多点触控** | ✅ 正常 | 点按、拖动、长按正常，稳定输入报告约 144 Hz，不能等同于传感器最高采样率 |
| **官方键盘 / 触控板** | ✅ 正常 | 磁吸 Pogo-pin 键盘打字与触控板指针操作正常 |
| **无线网络 (Wi-Fi)** | ✅ 正常 | PCIe 接口网卡正常驱动，已确认扫描热点和联网 |
| **蓝牙 (Bluetooth)** | ⚠️ 基础 | 固件加载正常，可扫描周边设备，配对与音频完善中 |
| **声音输出 (扬声器)** | ✅ 正常 | 左右立体声扬声器正常发声 |
| **声音输入 (麦克风)** | ⚠️ 有底噪 | 能录入声音，底噪偏大，电平调节待优化 |
| **前后相机** | ⚠️ 基础 | 两路已输出真实 ISP 帧，画质与桌面应用使用验证中 |
| **传感器** | ⚠️ 适配中 | SSC 通信已建立，物理读数与桌面联动仍在推进 |
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

## 构建与下载

在 GitHub Actions 的 **Build products** 中选择 `uefi`、`linux` 或 `debian-gnome`。成功后下载 `piano-目标-提交号` 并解压：里面有 `install.sh`、`install.cmd`、独立安装器和 `INSTALL.md`。`uefi` 包带唯一固件镜像；`debian-gnome` 包才带完整 ESP/root 磁盘包。日志与内核输出在另一个 `piano-build-records-目标-提交号` 中。

自行构建先取得固定源码，使用登记的构建容器。下面生成 UEFI 和可下载的安装工具包，不操作平板：

```sh
./build.sh sources
docker build -t sunuefi-builder -f containers/Dockerfile .
docker run --rm -v "$PWD:/workspace" -w /workspace sunuefi-builder bash -euc '
  git config --global --add safe.directory /workspace
  git config --global --add safe.directory "/workspace/*"
  python3 -m venv .venv
  .venv/bin/pip install -r requirements-build.txt
  ./build.sh uefi
  ./build.sh installer --product artifacts/product/PianoUEFI-product.img --output artifacts/installer-uefi
'
```

完整 Debian/GNOME 构建需要 **ARM64 构建机**和允许挂载/chroot 的构建容器；在同一环境内依次执行：

```sh
docker run --rm --privileged -v "$PWD:/workspace" -w /workspace sunuefi-builder bash -euc '
  git config --global --add safe.directory /workspace
  git config --global --add safe.directory "/workspace/*"
  ./build.sh linux
  ./build.sh mesa
  ./build.sh sensors
  ./build.sh release-rootfs
  ./build.sh package --root-size-mib 8192
  ./build.sh installer --bundle artifacts/release-7.2.9
'
```

这组命令需要前面已生成的 UEFI，rootfs 构建容器需 `--privileged`。单独固件工具包在 `artifacts/installer-uefi/`，完整包在 `artifacts/installer/`；依赖、来源和构建记录详见[公开构建链](docs/devel/public-build.md)。当前构建包仍是开发候选。

同一固定上游的基础根系统已构建完成后，可以用 `./build.sh release-rootfs --resume` 更新内核、硬件包和配置，复用基础系统，避免重新跑 debootstrap。此操作更新本地生成目录，不操作平板。

Debian/GNOME 的根系统构建会在独立副本中编译带 Piano 自动亮度策略的标准 `gnome-settings-daemon` 包，再按正常 APT 流程安装。原厂 lux 阈值和等待时间用于减少亮度频繁波动，不修改传感器读数；包来源和补丁记录随根系统保存。其他桌面直接使用标准传感器、背光接口。

维护者可用 `./build.sh trampoline --stock-boot 当前ROM的boot.img --output 新输出目录` 生成前置入口。选择器的 Android 直通已实测。`./build.sh boot-repack` 已能编译原生 BOOT 文件重打包／还原工具，真实原厂 BOOT 无损还原已验证；在线安装、OTA 自动化与请求自动消费尚未完成。`./build.sh module --inspect` 当前列出缺项，不生成可安装 ZIP。接口与升级流程见[Android 模块说明](docs/devel/android-module.md)，文件工具见[原生重打包说明](docs/devel/android-boot-repack.md)。

## 下载后如何刷写

电脑准备 Python 3.10+ 和 Android platform-tools，平板进入已解锁、开启 USB 调试的 rooted Android。Linux 使用 `sh install.sh`，Windows 将它换成 `install.cmd`。先用 `adb devices -l` 找到原机 ADB 序列号，明确选择自己的设备：

```sh
sh install.sh inspect --serial 原机ADB序列号 --output inspect.json
sh install.sh plan --serial 原机ADB序列号 --output update-plan.json
sh install.sh apply --serial 原机ADB序列号 --plan update-plan.json
```

默认是读取与核对，不会刷写。完整磁盘包自动使用随包的 `bundle/`。确认计划后，再明确允许执行：

```sh
sh install.sh apply --serial 原机ADB序列号 --plan update-plan.json --execute
```

当前只执行**已有 `sunuefi_esp` 和 `sunuefi_root` 的更新**，并在回 Android 后检查读回。出厂新平板首次缩小 userdata／创建分区尚未开放，脚本不能一键完成新机部署。Recovery 入口的依赖缺失仍待修复，安装器未开放刷写；UEFI-only 包也不含可执行磁盘更新的 ESP/root 镜像。需要临时体验固件或处理白屏时，参看[快速体验](docs/user/getting-started.md)和[返回 Android](docs/user/recovery.md)。

---

## 协议与来源

* UEFI 固件移植基于 [TianoCore EDK2](https://github.com/tianocore/edk2) 与 [Project Silicium (Mu-Silicium)](https://github.com/Project-Silicium/Mu-Silicium)，相关源码保留各自许可；本仓库包含 BSD、GPL、LGPL 等不同声明，具体范围见下方说明。
* 启动菜单前端基于 [simple-init](https://github.com/BigfootACA/simple-init)。
* 完整第三方来源与许可证见 [THIRD_PARTY.md](THIRD_PARTY.md) 与 [LICENSE.md](LICENSE.md)。
