# 公开构建链

统一入口是仓库根目录的 `build.sh`。源码由固定 submodule 提供，本项目的新增代码作为组件/overlay，已有源码改动通过标准 patch 和安装后的严格检查重现。

| 命令 | 当前作用 |
| --- | --- |
| `./build.sh sources` | 取得必要的固定上游源码，避免递归下载不参与编译的测试/fuzz资料 |
| `./build.sh check` | 运行明确登记的主机检查 |
| `./build.sh uefi` | 使用 vendor 板级材料与固件补丁构建唯一产品 |
| `./build.sh linux` | 重建公开基线+八个补丁的源码，编译 LABEL 根策略的内核与模块 |
| `./build.sh mesa` | 在 ARM64 Debian 构建容器中编译该发行版的 Piano Mesa 包 |
| `./build.sh rootfs --distro ID --desktop ID --plan` | 选择基础发行版与桌面，列出真实输入和包管理步骤；`--execute` 才构建 |
| `./build.sh package` | 从已完成且匹配的组件生成 ESP、root 归档和 manifest |
| `./build.sh installer --bundle artifacts/release` | 导出独立安装器、Linux/Windows 启动脚本、说明、校验记录和原样磁盘包 |
| `./build.sh install ...` | 独立安装入口，默认读取状态/计划，不随构建操作设备 |

UEFI 已在原准备环境中用新的 vendor 输入重建成功。内核完整构建、干净 rootfs/ESP 的端到端验证和 GitHub 实际运行分别记录，不把“命令已实现”当作全流程通过。

## 安装器当前范围

`inspect` 只读；`plan` 检查实际 GPT，识别已有 SunUEFI 分区或规划64GiB的新布局。已有专用分区可以显式 `apply --execute` 更新 ESP 和 ext4 根镜像，并检查读回。

专用根分区统一命名为 `sunuefi_root`。当前研究设备只做过 GPT 名称修改，UUID、边界与文件系统保持不变，ESP bootstrap已同步。

首次分区的执行目前返回 `NEW_INSTALL_NOT_READY`。需要先完成 Android 缩容 helper 与设备映射验证，才能开放文件系统缩小和 GPT 更新。Recovery 安装不随普通系统安装自动执行。

## 构建环境与发布

`containers/Dockerfile` 固定基础镜像；APT 实际包版本另记录，尚未固定完整软件仓库快照。CI 只构建和上传文件，不连接设备。失败输出带 `.incomplete` 或失败记录，安装器拒绝半成品。

UEFI 镜像、ESP 镜像与 root 归档是不同发布文件。所有发行版共用 UEFI 和板级内核，但编译型 Mesa/runtime 包必须匹配发行版、架构和 ABI；公共配置层可以复用。

## 下载包结构

构建与刷写快速步骤见[根 README](../../README.md#构建与下载)，下载后用法见[独立安装入口](../user/install-from-artifact.md)。

`build-products.yml` 的成功产物 `piano-TARGET-COMMIT` 直接包含 `install.sh`、`install.cmd`、原样复制的 `install_piano.py`、启动检查器、`INSTALL.md`、`installer-record.json` 和 `SHA256SUMS`。完整 `debian-gnome` 构建还包含 `bundle/` 下的原始 manifest 与磁盘镜像；日志和内核独立放在 `piano-build-records-TARGET-COMMIT`。

`uefi` 构建带 `PianoUEFI-product.img` 与 `uefi-manifest.json`，没有伪造的 ESP/root manifest；`linux` 构建的内核输出在记录包，安装工具本身不代表已构建完整系统。直接运行启动脚本只显示帮助，明确给出序列号后才检查设备，实际写入需要 `apply --execute`。首次分区与 Recovery 写入继续由原安装器拒绝。

main push 涉及 Linux/BSP、root、打包/安装器、公共构建配置或固定来源时，自动只选
`debian-gnome`，一次生成 UEFI、ESP/root 与安装脚本；该目标已经包含 UEFI/kernel，
不会再并行重复构建。纯 UEFI 来源/构建变化只选 `uefi`，普通文档变化不构建产品。
手动仍可明确选择 `uefi`、`linux` 或 `debian-gnome`；选择规则不代表远端构建已经成功。
过时构建按最终target取消，纯UEFI push不会取消正在运行的完整系统构建。

发布 initramfs 使用根系统从已认证 Debian APT 安装的 ARM64 `busybox-static`，
版本与摘要写入 `initramfs/busybox-source.json` 和 manifest。该包不带 `mountpoint`
applet，打包器同时收集发行版的 util-linux `mountpoint` 及 ELF 依赖；不再要求
本机历史 `build/linux-ram/busybox` 文件。新流程已在本地完成打包，尚未运行远端完整 CI。
