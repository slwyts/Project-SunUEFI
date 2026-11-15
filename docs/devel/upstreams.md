# 上游项目

SunUEFI 不是从零重写所有东西。上游源码固定在 `upstream/` 的指定提交上，我们自己的修改放在 `patches/`、`uefi/`、`linux/` 和构建工具里。下表列出每个上游负责什么，以及我们在它上面做了什么。

| 项目 | 负责什么 | 我们在上面做了什么 |
| --- | --- | --- |
| [TianoCore EDK2](https://github.com/tianocore/edk2) / [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | UEFI 基础代码和高通平台集成 | Piano 平台、启动交接、存储、输入、显示继承、UEFI Fastboot，以及对原厂分区的只读保护和安全退出 |
| [mu_wide_platforms](https://github.com/sunflower2333/mu_wide_platforms) | 基于 Project Mu 的高通设备 UEFI，包含小米平板 8 Pro 的 Piano 平台与 ACPI | 作为同机 UEFI 初始化、ACPI 和 Windows 引导适配的参考 |
| [Xiaomi Pad 6S Pro Linux](https://github.com/code002-2/Xiaomi-pad-6s-pro-Linux) / [debian-sheng](https://github.com/ianchb/debian-sheng) | 小米平板的多发行版适配，以及充电、手写笔和指纹方案 | 参考 MiPPS 认证、磁吸笔连接和 TEE 指纹接入，按 Piano 的实际硬件移植 |
| [simple-init](https://github.com/BigfootACA/simple-init) | UEFI 里的图形启动菜单 | 平板布局、中文、实体按键，以及和固件后台服务的联动 |
| [linux-piano](https://github.com/blu-sharky/linux-piano) | SM8750 内核、设备树和板级驱动基线 | 合入稳定版内核，修正内存 / DMA、显示、音频等。只取用其内容，不作为随仓库检出的源码 |
| [CachyOS/kernel-patches](https://github.com/CachyOS/kernel-patches) / [BORE](https://github.com/firelzrd/bore-scheduler) | 注重交互响应的 CPU 调度改进 | 将对应内核版本的 BORE 补丁集成到四个发行版共用的 ARM64 内核 |
| [librevm](https://github.com/code002-2/librevm) | Android Gunyah 驱动的主线接口适配 | 移植到当前内核，由 UEFI 传递每次启动的资源信息；虚拟机运行支持仍在调试 |
| [BigfootACA/linux](https://github.com/BigfootACA/linux) · Junhao Xie | 高通显示驱动改进 | 采用 DSI PLL 初始化、DPU 视频流水线停止顺序及 DSI PHY 接口时钟管理补丁，改善显示初始化与熄屏恢复；精确来源见 [THIRD_PARTY.md](../../THIRD_PARTY.md) |
| [debian-piano](https://github.com/blu-sharky/debian-piano) | Debian 根系统构建、设备服务、相机和输入辅助程序 | 专用 ESP / root、多发行版 BSP（板级配置包）、统一打包 |
| [piano-firmware](https://github.com/blu-sharky/piano-firmware) | Wi-Fi、蓝牙、DSP、触控等设备固件和功放预设 | 设备固件改为由官方 OTA 导入 `vendor/firmware`；它不是整套 HyperOS，也不含原厂相机算法 |
| [Mesa](https://mesa3d.org/) | Adreno 图形用户空间 | 四个系统共用官方 Mesa 27 开发源码与发行版原生打包 |
| [piano-sensors](https://github.com/blu-sharky/piano-sensors) | 高通传感器（SSC）通信与桌面桥接 | 数据类型、读取接口、标准 SensorProxy 与自动亮度策略 |
| [SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Valve ARM64 根系统提取方法和移动设备移植参考 | 复用固定版本提取器，接入 SunUEFI 的 ESP/root、内核及 BSP；不采用其设备启动器和分区方案 |
| [MiCode](https://github.com/MiCode) 与本机 HyperOS | 原厂设备树、驱动、配置的参照 | 对照真实命令、时序和参数，按标准接口移植 |

Mesa 使用 [`config/mesa.json`](../../config/mesa.json) 固定的官方 27 开发提交，四个系统从同一份源码生成各自的原生包。Adreno 830v1 的 drm/msm 芯片 ID `0xffff44050001` 已由上游 [MR43878](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43878) 合入，不再保留芯片 ID 和 D3D12 回移补丁，也不再引用外部 Mesa 包装仓库。开发提交没有发布签名，构建记录官方来源、固定提交及归档 SHA256；Debian 签名源码版本只用于取得发行版打包配方，不冒充所编译的 Mesa 版本。

SteamOS 仍需 `patches/mesa/27.0/` 的独立 renderonly 缓冲高度对齐修正。选定官方源码对非四行对齐的高度仍会少分配尾部空间，因此这项修复不随芯片 ID 补丁删除；Debian 和 Nyarch 的源码不应用它。

Debian 软件通过 APT 安装，Nyarch 和 SteamOS 使用原生 pacman 包。Nyarch 从 Arch Linux ARM 和应用源码构建，不依赖旧版 ISO。SteamOS 是实验目标。Ubuntu、Deepin 等配置暂未纳入默认构建。

各固定提交和许可证见 [THIRD_PARTY.md](../../THIRD_PARTY.md)、`.gitmodules` 和 `sources.lock.json`。
