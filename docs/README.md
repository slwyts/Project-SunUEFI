# 文档

[项目首页](../README.md)介绍项目和快速开始，[项目状态](status.md)列出每个功能现在能做什么。

## 我想用

| 页面 | 内容 |
| --- | --- |
| [快速上手](user/getting-started.md) | 第一次安装的完整路线 |
| [安装指南](user/install-from-artifact.md) | 在引导助手里分区、安装镜像、选择系统 |
| [电脑安装](user/install-from-computer.md) | 用电脑上的安装器更新已有分区 |
| [临时启动](user/temporary-boot.md) | 不改动平板，用 `fastboot boot` 试运行 UEFI |
| [故障恢复](user/recovery.md) | 回到 Android，处理启动失败 |
| [UEFI Fastboot](user/fastboot.md) | 读日志、截图、只读读取分区 |
| [已知问题](user/known-issues.md) | 现有限制和绕过办法 |
| [官方键盘](user/keyboard.md) | 没有 F 键时怎么按 |
| [HMCL 图形加速](user/hmcl.md) | 在 Linux 里运行 Minecraft 启动器 |

## 我想构建或贡献

| 页面 | 内容 |
| --- | --- |
| [架构说明](devel/architecture.md) | 从开机到桌面的启动链、分区布局与系统交接 |
| [UEFI 核心](devel/uefi-core.md) | UEFI 固件内部实现、安全退出与存储保护 |
| [文档写作规范](devel/writing-style.md) | 写作规则、词汇表和改写示例 |
| [上游项目](devel/upstreams.md) | 项目依赖的上游仓库与职责划分 |
| [构建手册](devel/building.md) | 环境、命令、产物位置、CI 下载 |
| [仓库地图](devel/repository-map.md) | 目录用途，哪些文件可以直接改 |
| [测试](devel/testing.md) | 主机检查和设备验证 |
| [发布要求](devel/release.md) | 发布前要核对的内容 |
| [本地输入](devel/local-inputs.md) | 完整构建需要的、不在仓库里的文件 |
| [补丁登记](devel/patches.md) | 补丁目录和选用规则 |
| [贡献指南](../CONTRIBUTING.md) | 提交规范和流程 |

按主题阅读实现说明：

| 目录 | 内容 |
| --- | --- |
| [`devel/boot/`](devel/boot/combined-boot.md) | 合体 BOOT、Android 模块、启动请求、UEFI 控制台和显示 |
| [`devel/hardware/`](devel/hardware/pen-protocol.md) | 显示、音频、相机、传感器、触控笔、电源等硬件的实现 |
| [`devel/linux/`](devel/linux/kernel.md) | 内核配置、系统服务、SteamOS |

发行版配方在 [BSP 配置包](../linux/bsp/README.md)、[根系统装配](../linux/rootfs/README.md)和[桌面配置](../linux/desktops/README.md)。第三方来源见 [THIRD_PARTY.md](../THIRD_PARTY.md)。
