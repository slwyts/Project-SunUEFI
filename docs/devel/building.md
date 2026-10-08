# 构建手册

所有构建都在电脑或 CI 上进行，不会向连接的平板写入。统一入口是仓库根目录的 `./build.sh`。

## 取得源码和检查

```sh
./build.sh sources     # 取得固定版本的上游子模块，不递归下载测试数据
./build.sh check       # 不需要平板的公开主机检查
```

上游源码放在 `upstream/`，保持固定提交。本地修改在 `patches/`、`uefi/`、`linux/`、`tools/`。构建在 `build/` 下的副本里应用补丁，不要改构建副本。

## 构建容器

容器基础镜像记录在 `config/build-container.json`，定义是 `containers/Dockerfile`：

```sh
docker build -t sunuefi-builder -f containers/Dockerfile .
```

APT 包版本会被记录，但不承诺位级可复现。

## UEFI 和安装工具

```sh
docker run --rm -v "$PWD:/workspace" -w /workspace sunuefi-builder bash -euc '
  git config --global --add safe.directory /workspace
  git config --global --add safe.directory "/workspace/*"
  python3 -m venv .venv
  source .venv/bin/activate
  python -m pip install -r requirements-build.txt
  ./build.sh uefi
  ./build.sh installer --product artifacts/product/PianoUEFI-product.img --output artifacts/installer-uefi
'
```

`./build.sh uefi` 使用 `vendor/piano` 的板级输入和 `patches/firmware`，输出 `artifacts/product/PianoUEFI-product.img` 与 `manifest.json`。完整 UEFI 构建还需要本地提取的原厂材料，见[本地输入](local-inputs.md)。

## 完整 Debian / GNOME

需要 ARM64 构建机，容器需要 `--privileged` 才能 chroot 并挂载：

```sh
./build.sh linux
./build.sh mesa
./build.sh sensors
./build.sh release-rootfs
./build.sh package --root-size-mib 8192
./build.sh installer --bundle artifacts/release-7.2.9
```

* 默认内核目标为 `7.2.9`：在 `debian-piano` 配套的内核基线上，合入官方 `v7.2.9` stable 提交和登记的 release 补丁。当前选择记录在 `build/release-7.2.9/source-manifest.json`。只准备源码：`python3 tools/prepare_release_kernel.py --refresh`。
* 重建内核时，旧产物清单先作废，编译完成后才原子发布新的，避免旧 Image 与新模块混用。O 目录与产物目录有文件锁。
* 基础根系统已构建后，用 `./build.sh release-rootfs --resume` 更新内核、硬件包和配置，不重新运行 debootstrap。
* 其他目标：`./build.sh rootfs --distro ID --desktop ID --plan` 列出 Ubuntu / Deepin / Arch 与 KDE 等组合的计划，目前只有 Debian / GNOME 完整验证。

### 软件包的本地构建

* **CPU 型号**：`linux/dts/piano-cpu-model.dtso` 给八个 CPU 节点加标准 `model` 属性，内核小补丁把它输出为 `/proc/cpuinfo` 的 `model name`，让 GNOME 显示 Snapdragon 8 Elite。MIDR、核心拓扑、时钟和板型号保持原值。
* **gnome-settings-daemon**（`./build.sh gsd`）：在副本里编译带 Piano 自动亮度策略的标准包，使用原厂 lux 阈值和延迟，不修改传感器读数，再经 APT 安装。
* **Mutter**（`./build.sh mutter`）：修正空 Gamma 曲线的恢复，以及无 EDID 的内置屏幕颜色配置。保留标准颜色服务和用户配置，不伪造 EDID；通用 sRGB 是未校准的默认值，不等于原厂色彩校准。来源与补丁见[Mutter 适配说明](../../linux/desktops/gnome/patches/mutter/README.md)。

## 合体 BOOT 相关工具

```sh
./build.sh trampoline --stock-boot 当前ROM的boot.img --output 输出目录   # 生成前置选择器入口
./build.sh boot-repack                                                   # 编译原生 BOOT 重打包 / 还原工具
./build.sh boot-request --sysroot ...                                    # 编译 piano-boot-request
./build.sh module --inspect                                              # 列出 Android 模块的缺项，不生成 ZIP
```

在线安装、OTA 自动化和请求自动清除尚未完成，见[原生 BOOT 重打包](android-boot-repack.md)、[Android 模块](android-module.md)。

## CI

GitHub Actions 的 **Build products** 提供 `uefi`、`linux`、`debian-gnome` 三个目标，产物为 `piano-目标-提交号`（含 `install.sh`、`install.cmd`、安装器和 `INSTALL.md`；`debian-gnome` 另有 `bundle/`），日志为 `piano-build-records-目标-提交号`。依赖和来源记录见[公开构建链](public-build.md)。

## 继续阅读

[仓库地图](repository-map.md)、[测试](testing.md)、[发布要求](release.md)、[贡献指南](../../CONTRIBUTING.md)。
