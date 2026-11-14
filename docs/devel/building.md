# 构建手册

在电脑或 CI 上用 `./build.sh` 构建 UEFI 镜像、Android 引导助手和各发行版的系统镜像。所有构建都只在本机和容器里进行，不会向连接的平板写入。

## 环境要求

| 目标 | 构建机 | 容器权限 |
| --- | --- | --- |
| UEFI 镜像、Android 引导助手模块 | x86_64 或 ARM64 的 Linux，装有 Docker | 普通容器 |
| 完整系统镜像（Debian GNOME / KDE、Nyarch GNOME、SteamOS） | ARM64 的 Linux，装有 Docker | `--privileged`，用于 chroot 和挂载 |

GitHub Actions 的 `uefi` 和 `linux` 目标在 x86_64 runner 上构建，四个发行版目标在 ARM64 runner 上构建。

## 取得源码和主机检查

```sh
./build.sh sources     # 取得固定版本的上游子模块，不递归下载测试数据
./build.sh check       # 主机检查，不需要平板
```

上游源码放在 `upstream/`，固定在指定提交。本地修改在 `patches/`、`uefi/`、`linux/`、`tools/`。构建在 `build/` 下的副本里应用补丁，不要改构建副本。

## 构建容器

基础镜像记录在 `config/build-container.json`，定义是 `containers/Dockerfile`。Debian 基础镜像优先从 ECR Public 下载，元数据下载失败时改用 Docker Hub，两个来源固定同一个 OCI 摘要。

```sh
python3 tools/build_container.py     # 生成镜像 sunuefi-builder
```

APT 包版本会被记录，但构建结果不保证逐字节一致。

## UEFI 和引导助手

```sh
docker run --rm -v "$PWD:/workspace" -w /workspace sunuefi-builder bash -euc '
  git config --global --add safe.directory /workspace
  git config --global --add safe.directory "/workspace/*"
  python3 -m venv .venv
  source .venv/bin/activate
  python -m pip install -r requirements-build.txt
  ./build.sh uefi
  ./build.sh module
'
```

`./build.sh uefi` 使用 `vendor/piano` 的板级输入和 `patches/firmware`，输出 `artifacts/product/PianoUEFI-product.img` 和 `manifest.json`。完整 UEFI 构建还需要本地提取的原厂材料，见[本地输入](local-inputs.md)。

`./build.sh module` 读取刚构建的产品，编译前置选择器和六个静态 ARM64 工具，生成 `artifacts/android/SunUEFI-Piano-版本.zip`、`manifest.json` 和 `SHA256SUMS`。它不需要平板、原厂 BOOT 或私有采集目录，Android 内核在模块安装时从设备当前 BOOT 提取。默认从经过签名校验的 Debian ARM64 软件包准备编译用的 sysroot，不执行其中的 ARM64 程序。输入和安装行为见 [Android 引导助手](boot/android-module.md)。

合体 BOOT 的底层工具单独提供：

```sh
./build.sh trampoline --stock-boot 当前ROM的boot.img --output 输出目录   # 生成前置选择器入口
./build.sh boot-repack                                                   # 编译原生 BOOT 重打包 / 还原工具
./build.sh boot-request --sysroot ...                                    # 编译 piano-boot-request
./build.sh module-package --inspect                                      # 模块打包工具，只检查已有输入
```

原理见[合体 BOOT](boot/combined-boot.md)。

## 完整系统

四个目标共用同一份内核和 UEFI，各自构建根系统：

| 目标 | 根系统 | 构建目录 |
| --- | --- | --- |
| `debian-gnome` | Debian sid + GNOME | `build/distros/release` |
| `debian-kde` | Debian sid + KDE Plasma | `build/distros/release` |
| `nyarch-gnome` | Arch Linux ARM + GNOME（Nyarch） | `build/distros/nyarch-gnome-piano` |
| `steamos` | Valve ARM64 SteamOS | `build/distros/steamos-piano` |

默认内核是 Linux `7.3`，版本后缀 `-piano-sunuefi-cachy`。硬件适配、Gunyah 和对应版本的 CachyOS BORE 调度补丁按主题维护，固定输入见 [`patches/linux/7.3/series.json`](../../patches/linux/7.3/series.json)，当前选择记录在 `build/release/source-manifest.json`。内核和模块输出到 `artifacts/kernels/release/`。只准备内核源码用 `python3 tools/prepare_release_kernel.py --refresh`。具体改动见[内核与调度配置](linux/kernel.md)。

重建内核时，旧的产物清单先作废，编译完成后才原子发布新的，避免旧 Image 配新模块。

### Debian GNOME 和 Debian KDE

```sh
./build.sh linux
./build.sh release-rootfs                    # KDE 加 --desktop kde
./build.sh package --root-size-mib 8192
python3 tools/package_sparse_images.py --bundle artifacts/release --output artifacts/fastboot-sparse
```

已有根系统时，运行 `./build.sh esp` 只生成启动镜像，不重新导出 root 镜像。
启动镜像包含 `/EFI/Linux/sunuefi.efi`：把内核、initramfs、DTB 和启动参数
封装为一个 UKI，由 SunUEFI 直接启动，不经过 systemd-boot。打包使用
systemd 的 `ukify`，默认从根系统的 `systemd-boot-efi` 包取 ARM64 stub；
这个包名不表示镜像安装了 systemd-boot 管理器。单独提取的 stub 可通过
`--stub /path/to/linuxaa64.efi.stub` 指定，`--ukify` 指定打包工具。

基础根系统已经构建过时，用 `./build.sh release-rootfs --resume` 更新内核、硬件包和配置，不重新运行 debootstrap。`./build.sh rootfs --distro ID --desktop ID --plan` 会列出其他发行版和桌面组合的计划，目前只有上表的四个目标能完整构建。

内核和桌面包的并行度默认按 CPU 配额与可用内存选择，可用 `--jobs` 覆盖。
桌面包共用 `build/ccache`（或 `CCACHE_DIR`），FFmpeg、Mutter、GSD 和 GDM
会先校验并复用完整运行包。FFmpeg 构建中断后加 `--resume`，保留同一源码的对象文件。
x86 主机用 `./build.sh ffmpeg --sysroot 已完成的sid_ARM64根目录` 构建：编译器、链接器
和 make 在宿主 CPU 原生运行，头文件与库来自 ARM64 sysroot；QEMU 仅执行目标程序。
其他桌面包的 `--sysroot`／`--rootfs` 入口使用相同的原生编译工具。
宿主需安装 Clang、LLVM、AArch64 binutils、patchelf 和 ccache；Ninja 项目安装宿主 Ninja。
交叉编译使用 GNU 汇编器与链接器，保留 Debian 的符号版本表和调试信息处理。
产品 CI 使用原生 ARM runner，缓存与源码、补丁匹配的 Debian 运行包，并取消被新提交替代的构建。

Debian 的 Mesa、FFmpeg、sensors 在 sid 根目录的隔离副本中构建，不安装宿主容器的库或混用 trixie-backports。`--sysroot` 不修改输入根目录；包版本、源码归档、补丁和二进制哈希记录在各输出目录。三个产品共用 `config/mesa.json` 固定的官方 Mesa 27 开发源码；Debian 保留 sid 原始完整配方，Nyarch 使用 Arch Linux ARM 的完整配方。官方源码已包含 830v1 和 DirectX-Headers 修正，旧26.2回移植已删除。SteamOS 的 renderonly 高度修正仍单独保留，因为该官方提交只对齐缓冲宽度，不保证非4行对齐模式的尾部空间。

已有 sid ARM64 根目录时，可以单独重建这三组包：

```sh
sudo ./build.sh mesa --sysroot build/distros/debian-sid/rootfs
sudo ./build.sh sensors --sysroot build/distros/debian-sid/rootfs
sudo ./build.sh ffmpeg --sysroot build/distros/debian-sid/rootfs
```

### 共用构建与缓存

Build products 先准备一次 ARM64 构建容器，再并行准备两类共享产物：UEFI 与 Android 模块、内核与模块。四个 root job 等待这些阶段完成，恢复同一代产物并校验后才构建各自系统，不再分别编译四遍 UEFI、模块和内核。内部产物放 Actions cache，不增加用户下载项；共享阶段的日志合并进原有 `logs.zip`。

完成缓存只在构建、源码和输出校验成功后发布。缓存键包含构建器、绑定源码、补丁、配置、架构、工作区路径及实际容器镜像 ID。内核恢复保留 Image、完整模块集及外部模块构建需要的配置、符号表和头文件；原生内核入口仍核对 SOURCE、编译器、配置、模块 hash 和 vermagic。root job 检查不匹配时停止，不自行重新编译或混用旧产物。UEFI 缓存保留原有构建标记、同核心模块身份与完整文件摘要，不把未完成的构建标为完成。

内核只绑定 `config/release.json` 的 `kernel` 子对象，按规范化 JSON 记录内容与 SHA256；修改 sensors、Mesa、登录或其他根系统配置不使内核缓存失效，内核配置、源码与工具链变化仍会失效。旧内核清单只保存整份配置文件摘要，无法还原当时的子对象，需要正常构建一次生成新记录；不会改写旧清单来跳过构建。

固定 LLVM 归档、固件中间对象及 ccache 分开缓存，避免每次下载工具链或因小改动完全丢失对象缓存。只保存 LLVM 压缩归档，不把展开后十余 GiB 的目录占满 Actions cache。构建容器按 Dockerfile、固定基础镜像及构建器区分，载入时核对归档摘要和 OCI 镜像 ID。每种共享缓存只有一个写入 job，本地入口通过文件锁串行更新并原子发布缓存记录。

本地需要连续构建多个发行版时，在已准备好的构建环境里使用同一入口：

```sh
python3 tools/build/shared_products.py ensure --kind firmware
python3 tools/build/shared_products.py ensure --kind kernel
# 后续只校验共用产物；输入不匹配则停止
python3 tools/build/shared_products.py verify --kind firmware
python3 tools/build/shared_products.py verify --kind kernel
```

这些命令只由 CI 或维护者的本地共用构建调用，不改变 rootfs 的安装过程。sid 和 Nyarch 的滚动仓库仍每次读取并校验签名索引、选择实际 candidate；运行包继续由原构建器核对 `SOURCE.json`、依赖 ABI 与文件摘要，缓存不冻结候选版本。sensors 与 FFmpeg 缓存保留 `runtime/`、`SOURCE.json` 和 `SHA256SUMS`；旧版 `package-cache.json` 不再参与 sensors 复用。缓存键包含公共 sid 构建器 `tools/build/debian.py`，构建器变化后不会继续使用同一配方缓存。

官方 Mesa 27 的 Debian 缓存另外保留 `build/mesa/runtime/`、`SOURCE.json`、`SHA256SUMS` 和原始 `.changes` 收据，不缓存 builder。Mesa 自己的键绑定 `config/mesa.json` 中的官方源码提交与归档摘要、构建器、编译环境和验证器；更新 Mesa pin 不改变其他包的配方键。恢复后仍核对目标 APT 候选与 ABI、签名 sid 中的 Debian 配方源码摘要、官方源码 pin，以及 `.changes` 和九个运行包的身份与摘要，不缓存 APT 索引或跳过验证。

Debian 包按配方前缀恢复最近缓存，成功后发布本次运行的一代，避免 Actions 同名缓存不可覆盖导致候选更新后每次重编；公共包只由选定的一个 Debian job 保存。完整 root 镜像不存入构建缓存。冷编译、签名索引刷新、根系统安装和镜像导出仍需要时间，不能仅凭缓存设计保证整套产品在几分钟内完成。

两个 Debian 桌面还复用 `build/distros/release/fingerprint-packages/` 的 `SOURCE.json` 与 `runtime/`，不缓存指纹 package builder。复用前仍核对原生后端源码归档、libfprint 补丁和配置、目标库 ABI 与实际 `.deb` 摘要，并检查目标 fprintd 的动态链接和 APT 依赖；缓存未命中时正常构建。

本地和 GitHub 的完整构建走同一个 `release-rootfs` 入口。默认系统包含中文界面、拼音输入法、Chromium、GNOME 动画、蓝牙地址自动初始化和触控笔组件。根系统生成后会检查这些软件包和必需文件，结果记录在 `manifest.json` 的 `default_system`，缺少任何一项构建就失败。

Debian 只构建 sid，不混用 stable 或 backports。`release-rootfs` 先生成 sid 基础系统，再从其副本构建 sensors、FFmpeg 和桌面包；不用固件构建容器中的旧版库编译这些运行包。CI 保留编译缓存，但按 sid 配置和构建器区分缓存，并由软件包构建器检查所选源码版本后复用。

### Nyarch GNOME 和 SteamOS

```sh
./build.sh uefi && ./build.sh module && ./build.sh linux
./build.sh nyarch-gnome-rootfs               # SteamOS 用 steamos-rootfs
./build.sh package \
  --rootfs build/distros/nyarch-gnome-piano/rootfs \
  --rootfs-manifest build/distros/nyarch-gnome-piano/manifest.json \
  --initramfs build/distros/nyarch-gnome-piano/initramfs/initramfs.cpio.gz \
  --root-size-mib 16384
python3 tools/package_sparse_images.py --bundle artifacts/release --output artifacts/fastboot-sparse
```

SteamOS 把命令里的 `nyarch-gnome-piano` 换成 `steamos-piano`。Nyarch 从 Arch Linux ARM 基础系统开始，通过签名仓库安装软件包，再构建 Nyarch 应用和 Piano 的原生硬件包，源码和扩展选择在 `config/nyarch.json`。SteamOS 的构建细节见 [SteamOS ARM64](linux/steamos.md)。

已有隔离的 ARM64 pacman 构建根目录时，可单独构建并安装完整 Mesa 包；此操作会修改指定的构建根目录，不要直接使用已交付的产品根目录：

```sh
sudo ./build.sh mesa build/mesa-nyarch --format pacman --rootfs build/distros/nyarch-builder/rootfs --jobs 8
sudo ./build.sh mesa build/mesa-steamos --format pacman --rootfs build/distros/steamos-builder/rootfs --steamos --jobs 8
```

中断后使用相同参数加 `--resume`，保留已配置的源码和对象。SteamOS 构建保留 `deckard-mesa-linux-aarch64` 包名及 renderonly 高度补丁；Nyarch 保留 Arch Linux ARM 的原生拆分包。

所有发行版都由 `tools/stage_piano_firmware.py` 安装同一套设备固件。`config/release.json` 的 `firmware.overlays` 选用完整的 HyperOS 4 ADSP 和 ADSP-DTB 更新，覆盖旧文件，来源、大小和 SHA256 记录在固件清单里。

### 单独构建的软件包

CPU 型号由 `linux/dts/piano-cpu-model.dtso` 给八个 CPU 节点加标准 `model` 属性，内核补丁把它输出为 `/proc/cpuinfo` 的 `model name`，让 GNOME 显示 Snapdragon 8 Elite。MIDR、核心拓扑、时钟和板型号保持原值。

`./build.sh gsd --sysroot 已完成的基础根目录` 在独立副本里编译带 Piano 自动亮度策略的 gnome-settings-daemon，使用原厂 lux 阈值和延迟，不修改传感器读数，由 APT 安装。默认的 `release-rootfs` 会自动完成这一步，策略只匹配 `xiaomi,piano` 板级标识。

`./build.sh mutter` 从签名 sid 索引选择 Mutter 源码，与 Nyarch 共用空 Gamma 曲线恢复和已取消触摸 grab 两项修复；无 EDID 的标准 sRGB fallback 已由发行版提供，不再重复打补丁。标准颜色服务和用户配置保持不变。来源与补丁见 [Mutter 适配说明](../../linux/desktops/gnome/patches/mutter/README.md)。
单独构建时使用了其他输出目录，可通过 `release-rootfs --mutter-dir 输出目录/runtime` 选择；组装前会检查源码、补丁和软件包摘要。

## 产物位置

| 目录 | 内容 |
| --- | --- |
| `artifacts/product/` | `PianoUEFI-product.img` 和 `manifest.json` |
| `artifacts/android/` | 引导助手模块 `SunUEFI-Piano-版本.zip`、`manifest.json`、`SHA256SUMS` |
| `artifacts/kernels/release/` | 内核 Image、模块和 `manifest.json` |
| `artifacts/release/` | `package` 的输出：`esp.img`、`root.ext4.img` 和 `manifest.json` |
| `artifacts/fastboot-sparse/` | 稀疏版 `esp.img`、`root.ext4.img`，以及 `manifest.json` 和 `SHA256SUMS` |
| `artifacts/installer/` | `installer` 的默认输出目录 |

## CI 下载

GitHub Actions 的 **Build products** 在 `main` 上有相关改动时自动运行，也可以手动选择 `all`、`uefi`、`linux` 或任一发行版。只改文档不会触发构建，唯一的例外是 `docs/user/install-from-artifact.md`，改动它会触发整套产品构建。选择规则在 `tools/ci_build_targets.py`。

| 下载名称 | 内容 |
| --- | --- |
| `SunUEFI-Module` | Android 引导助手模块 ZIP |
| `SunUEFI-UEFI-Fastboot` | 用于 `fastboot boot` 的独立 UEFI 镜像 |
| `SunUEFI-ESP` | 各发行版共用的稀疏 ESP 镜像 |
| `SunUEFI-Root-发行版` | 对应发行版的稀疏 root 镜像，发行版为 `debian-gnome`、`debian-kde`、`nyarch-gnome`、`steamos` |
| `logs` | 合并的构建日志、来源清单和校验记录 |

模块、UEFI 镜像和 ESP 每次只上传一份，root 镜像按发行版分别上传。安装时把所选发行版的 root 镜像和同一次构建的 ESP 配套使用，步骤见[安装指南](../user/install-from-artifact.md)。

## 安装器

`./build.sh installer` 和 `./build.sh install` 是两件事。

`installer` 只在电脑上整理文件。它把 `install_piano.py`、启动脚本 `install.sh` 和 `install.cmd`、`INSTALL.md`、校验记录复制到一个新目录，`--bundle artifacts/release` 会把磁盘镜像一并放进 `bundle/`，`--product` 则只放 UEFI 镜像。目录必须是新的，已有的目录会被拒绝。

`install` 才会连接平板，由 `tools/install_piano.py` 提供三个操作：

| 操作 | 作用 |
| --- | --- |
| `inspect` | 只读检查平板的分区表（默认） |
| `plan` | 检查平板和镜像，生成安装计划 |
| `apply --plan 计划.json --bundle 镜像目录 --execute` | 更新已有的 `sunuefi_esp` 和 `sunuefi_root`，并读回校验 |

命令行安装器只更新已有的独立分区，首次分区在引导助手里完成。不带 `--execute` 时不会写入任何内容。

## 继续阅读

[仓库地图](repository-map.md)、[测试](testing.md)、[发布要求](release.md)、[贡献指南](../../CONTRIBUTING.md)。
