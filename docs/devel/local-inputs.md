# 本地输入与复现边界

本页列出完整构建所需的本地输入。代码仓库不包含被 `.gitignore` 忽略的文件；下表说明每个构建目标需要哪些外部输入，以及由哪个脚本读取或核对。

## 按目标选择输入

| 目标 | 必需输入 | 校验来源 | 准备方式 |
| --- | --- | --- | --- |
| 产品固件 | `vendor/piano/`（板级 DTB、xbl-config DTB、原生 PE/DEPEX） | `tools/piano_vendor_inputs.py` 核对哈希 | 仓库已内置清理后的板级文件；通过 `python3 tools/piano_vendor_inputs.py` 验证 |
| 救援 RAM 内核 | 对应提交的内核源码、config、编译器，静态 ARM64 BusyBox | `linux/kernel-profiles.json` | `python3 tools/build_kernel.py` |
| 完整 Linux 内核 | 干净的内核 worktree、公共配置、编译器 | `build/release/source-manifest.json` | `./build.sh linux` |
| 完整设备树 | ROM base DTB、live DTB、overlay、dtc/fdtoverlay | `tools/build_piano_full_dtb.py` | `python3 tools/compose_piano_dtb.py` |
| 磁盘根系统与 ESP | ARM64 根文件系统、内核与模块、BusyBox、DTB | `tools/package_release.py` 配套校验 | `./build.sh package` |

## 原厂提取与原生模块

- `tools/dev/capture_device.py` 从已取得 root 权限的平板提取原厂分区与设备树，需要指定 `--output` 和 `--serial`。
- `vendor/piano/` 中的原生 PE、DEPEX 和 DTB 已经过清理，移除了主机专属的序列号、种子和启动参数，由 `tools/piano_vendor_inputs.py` 固定哈希并核对。
- 历史 GPT 采集仅供测试重放使用，产品固件直接读取当前设备的 GPT，不与历史整表快照比对。

## 环境与字体

固件构建环境使用 `upstream/Mu-Silicium`、`.venv` 以及 `tools/build_stage0.sh` 的 BaseTools 补丁。字体来源与校验值见[第三方来源](../../THIRD_PARTY.md)。
