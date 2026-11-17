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
systemd 的 `ukify`，默认使用 `config/release.json` 固定的 Debian ARM64
`systemd-boot-efi` 包中的 stub，校验包和文件的 SHA256，四产品使用同一文件；
这不会在目标系统安装 Debian 软件包或 systemd-boot 管理器。单独提取的 stub 可通过
`--stub /path/to/linuxaa64.efi.stub` 指定，`--ukify` 指定打包工具。

initramfs 的 BusyBox、分区识别工具和依赖库也由 `config/release.json` 固定并校验，
在临时目录解包，不从各发行版根系统取库。内核模块和键盘固件使用共享产品的同一份输入，
因此同次构建的四个系统可以使用同一个启动镜像。

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

两种产品构建都会检查 Mesa 源码、完整驱动选项和 ARM64 包。CI 分别缓存原生包；复用前核对来源、配方与包摘要，安装后重新检查 ELF 和目标系统的依赖，避免把不匹配的滚动发行版缓存带入镜像。

所有发行版都由 `tools/stage_piano_firmware.py` 安装同一套设备固件。`config/release.json` 的 `firmware.overlays` 选用完整的 HyperOS 4 ADSP 和 ADSP-DTB 更新，覆盖旧文件，来源、大小和 SHA256 记录在固件清单里。

### 单独构建的软件包

CPU 型号由 `linux/dts/piano-cpu-model.dtso` 给八个 CPU 节点加标准 `model` 属性，内核补丁把它输出为 `/proc/cpuinfo` 的 `model name`，让 GNOME 显示 Snapdragon 8 Elite。MIDR、核心拓扑、时钟和板型号保持原值。

`./build.sh gsd --sysroot 已完成的基础根目录` 在独立副本里编译带 Piano 自动亮度策略的 gnome-settings-daemon，使用原厂 lux 阈值和延迟，不修改传感器读数，由 APT 安装。默认的 `release-rootfs` 会自动完成这一步，策略只匹配 `xiaomi,piano` 板级标识。

`./build.sh mutter` 从签名 sid 索引选择 Mutter 源码，与 Nyarch 共用空 Gamma 曲线恢复和已取消触摸 grab 两项修复；无 EDID 的标准 sRGB fallback 已由发行版提供，不再重复打补丁。标准颜色服务和用户配置保持不变。来源与补丁见 [Mutter 适配说明](../../linux/desktops/gnome/patches/mutter/README.md)。
单独构建时使用了其他输出目录，可通过 `release-rootfs --mutter-dir 输出目录/runtime` 选择；组装前会检查源码、补丁和软件包摘要。

## 软件包与更新标准化

硬件支持应能安装到干净的受支持发行版中，再通过发行版包管理器升级、卸载和
回退。四个产品镜像是这些软件包和首次安装默认设置的组合，不应成为硬件适配
唯一能生效的环境。下面区分已经使用的原生包和仍由构建脚本复制的文件；
未完成的实施工作见[发布要求中的开发清单](release.md#未完成的开发工作)。

| 内容 | 现有安装方式 | 标准化方向 |
| --- | --- | --- |
| Linux 内核与模块 | `Image`、配置、`System.map` 和模块目录；rootfs 构建器直接复制模块 | Debian 内核包与 pacman 内核包，随包安装并维护匹配的模块 |
| 设备树 | ESP 打包时组合板级 DTB、overlay 和绑定转换 | 构建匹配内核的 DTB，按内核版本安装，生成 UKI 时选取同版文件 |
| Linux 设备固件 | `tools/stage_piano_firmware.py` 校验后复制到 `/usr/lib/firmware` | 原生固件包，补充发行版缺少的文件，避免与 `linux-firmware` 重复拥有同一路径 |
| ALSA UCM、AudioReach topology、udev、笔校准及硬件配置 | 已有 BSP 打包入口，完整产品仍有直接复制 | 明确包内文件、依赖、配置覆盖和卸载范围 |
| Mesa、FFmpeg | 已有 Debian 和 pacman 原生包 | 保留目标发行版完整配方、功能和 ABI，按软件源正常更新 |
| 传感器 | Debian 提供 FastRPC、libssc、iio-sensor-proxy 和 Piano 配置的六个运行包；Arch 部分集成文件另行复制 | 原生包包含完整集成文件，运行时继续提供标准 SensorProxy 接口 |
| PipeWire 相机插件 | 已有 GStreamer 插件包和 `pipewire-v4l2-source` 包 | 保留明确的 PipeWire ABI 依赖，修复进入上游后撤掉本地替代 |
| 指纹 | libfprint／fprintd 使用目标发行版包；CLI 和服务配置另行安装 | 正常包拥有全部必要文件，标准认证结果仍取自 libfprint／fprintd |
| 笔及相机工具、外部模块 | runtime 文件集合直接安装，包含匹配内核的 `v4l2loopback` | 运行组件、诊断工具和内核模块分别打包，标准替代完整可用后删除旧路径 |
| GNOME／KDE／SteamOS 组件 | Mutter、GDM、GSD、libwacom、Gamescope 等已有原生包，默认设置和会话文件仍有 overlay | 通用修复维护一份补丁，设备配置与产品默认值分开，不覆盖用户设置 |

Linux 设备固件包括 ADSP、CDSP、Goodix 签名程序及模型、ath12k、蓝牙、GPU、
VPU、内屏、键盘和触屏文件，来源由 `config/release.json` 和组件清单记录。
原厂固件的许可与再分发条件需逐项核对，不能由本项目源码许可覆盖。
SunUEFI 固件和 Android 引导助手使用另一条更新链；安装 Linux 内核或硬件包
不应顺带覆盖 Android BOOT。

### 内核软件包

Debian 优先复用选定内核的 `make bindeb-pkg`，提供按 ABI 版本命名的
`linux-image-<release>`、`linux-headers-<release>`，再由元包依赖最新版本。
Arch 系使用 `PKGBUILD`／`makepkg`，分别提供设备内核和 headers 包；不得将
设备内核声明为可满足发行版原版 `linux` 的二进制模块依赖。
共用源码、补丁、配置和版本定义，不把 `.deb` 当作 pacman 包安装。

内核 release 必须区分会影响模块兼容性的修改，避免不同驱动占用同一个
`/usr/lib/modules/<release>/`。模块与内核的构建配置、导出符号必须匹配，
不能只比较 `vermagic`。headers 包需包含外部模块构建所需的生成头文件、
配置、脚本和 `Module.symvers`；`make headers_install` 导出的用户空间
接口头文件不能替代它，也不需要因安装设备内核就替换发行版 `linux-libc-dev`。
交叉构建时还需检查 headers 内附工具能在目标机运行。

DTB 与内核一同构建和按版本安装，避免旧内核使用被覆盖的新设备树。
长期把板级描述收敛到内核的 `arch/arm64/boot/dts/qcom/`，减少外部转换；
同次启动的动态内存和固件资源仍由 UEFI 通过标准设备树修正协议提供。

### 从装包到 UKI

正常升级应由以下流程完成，而不是重新刷写整个 ESP：

```text
APT／pacman 安装内核、模块、匹配 DTB 和必要固件
  → 处理外部模块及 depmod
  → 发行版 initramfs 工具生成对应版本的 initramfs
  → ukify／mkinitcpio 生成 UKI
  → 安装到 ESP 的 EFI/Linux/
  → 引导程序发现启动项，或由标准 Boot#### 直接启动
```

Debian 遵循 `/etc/kernel/` 和 `/etc/initramfs/post-update.d/` 的钩子约定；
Arch 遵循 mkinitcpio preset 和 pacman hooks。选用 `kernel-install` 时可用
`layout=uki` 组织安装，不再让另一套钩子重复生成。系统名称来自目标系统的
`os-release`，根设备使用标准 `root=UUID=…` 或 `root=PARTUUID=…`，逐步
替换私有 `piano.root=` 和固定分区名检查。Android 数据保护继续保留。

UKI 按安装标识及内核版本命名，不能依赖固定的 `sunuefi.efi` 或 `linux.efi`。
多个系统应能独立安装自己的 UKI，SimpleInit 和 systemd-boot 都只是可选
引导程序。四产品可以共用内核和固件，安装后的 UKI 不必逐字节相同；
共用初装镜像不能限制各发行版自己的启动参数、名称和后续更新。

回退必须同时保留旧 UKI、模块和 DTB。Debian 的按 ABI 包名便于并存；
Arch 的同名内核包升级不会自动保留旧模块，需要明确的并存或配套降级方案。
`fallback` initramfs 不是旧版本内核。生成失败、ESP 未挂载或空间不足时应
报告失败并保留原启动组合，不先删除旧内核。不得在包卸载时删除其他系统的启动项。

### 软件源与文件归属

建立签名 APT 和 pacman 软件源，Nyarch 与 SteamOS 按各自库 ABI 构建和发布，
用正常依赖及版本更新替代 SteamOS 对适配包的长期 `IgnorePkg`。
包内程序不放进留给管理员的 `/usr/local`；默认配置、服务和状态目录使用
发行版约定，升级保留密码、机器身份、配对、游戏和用户修改。
普通用户空间组件只依赖实际使用的接口与库版本，不因内核重新构建就全部重编。

软件包管理与硬件接口标准化分别完成。把现有相机 daemon／loopback 或指纹
UIO 后端打成原生包，不等于标准 CAMSS／libcamera 或上游指纹控制模型已经完成。
源码和配方随包可取得，通用补丁被上游收录后删除本地副本。

R2 软件源顶层为 `apt/`、`pacman/`、`keys/`，公开入口是
`https://sunuefi-repo.okassets.uk`。APT 使用 `apt/dists/sid` 和
`apt/pool/main`。pacman 遵循 `$repo/os/$arch`：
`sunuefi` 提供共用的内核、DTB、设备配置和 Arch Linux ARM 重建包。
Nyarch 只启用 `sunuefi`；SteamOS 把 `sunuefi-steamos` 放在 `sunuefi` 前，
只有为 Valve 用户空间重建的包放入增量仓。同名包优先使用排在前面的仓库，
没有覆盖的包从 `sunuefi` 取得。

`./build.sh repo --repo apt|sunuefi|sunuefi-steamos
--key <指纹> --output <新目录> <包文件...>` 生成签名索引，
加 `--publish` 后使用 `R2_ENDPOINT`、`R2_BUCKET`、`R2_ACCESS_KEY_ID` 和
`R2_SECRET_ACCESS_KEY` 上传。上传先完成不可变包文件和签名，再更新索引，
同名文件内容变化即拒绝；保留旧包，不执行远端删除。
APT 发布 by-hash 索引，最后更新 `InRelease`；下次发布从这份签名索引继续。
pacman 数据库和分离签名是不同对象，发布期间可能短暂验签失败，应重试，
不能关闭签名检查。上传中断时，发布工具仍可从上一份完整索引继续。

依据：[Debian 内核包与钩子](https://kernel-team.pages.debian.net/kernel-handbook/ch-update-hooks.html)、
[Arch 内核打包](https://wiki.archlinux.org/title/Kernel/Arch_build_system)、
[`kernel-install`](https://www.freedesktop.org/software/systemd/man/latest/kernel-install.html)。

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
