# 贡献指南

Project SunUEFI 为小米平板 8 Pro（`piano`）提供统一 UEFI 和 Linux 适配，长期目标包括 Windows on ARM。欢迎提交问题、测试结果和代码。

## 反馈问题

在 GitHub Issues 里写清楚：使用的构建（提交号或镜像哈希）、做了什么、看到什么。如果 UEFI Fastboot 可用，附上 `fastboot -s SunUEFI-piano oem ramlog` 和 `get_staged` 导出的日志；Linux 里附上 `dmesg` 和相关服务的 `journalctl`。同时说明你的面板和内存容量；当前运行使用 CSOT 配置。

## 提交代码

1. 拆成小而清楚的 Pull Request，说明为什么改和怎样验证。
2. 目录：UEFI 在 `uefi/`、`patches/`；内核配置、设备树和用户空间在 `linux/`；Android 一侧工具在 `android/`。详见[仓库地图](docs/devel/repository-map.md)。
3. 提交前运行 `./build.sh check`，构建方法见[构建手册](docs/devel/building.md)。
4. 提交信息使用 Conventional Commits，如 `fix(uefi): 修复……`、`feat(linux): ……`。
5. 使用 AI 编程助手时遵循 [AGENTS.md](AGENTS.md)。

## 约定

* **一套核心**：新功能和修复都进同一套 UEFI 和同一个产品镜像，不另做独立的测试镜像。
* **保护原厂数据**：涉及 UFS 读写、内存映射和 `ExitBootServices` 的改动，必须保留原厂分区只读保护和资源安全退出。
* **标准接口优先**：硬件支持尽量放在内核驱动和设备树里，让干净的发行版也能用；桌面联动用 BlueZ、ALSA/UCM、IIO、DRM/KMS、UPower 等标准接口。
* **写清楚做到哪**：区分“代码已编译”“驱动已加载”和“在平板上确实可用”，没做完的明确写出来。
