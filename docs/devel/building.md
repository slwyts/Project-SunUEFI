# 构建手册

在电脑或 CI 上用 `./build.sh` 构建 UEFI 镜像、Android 引导助手和各发行版的系统镜像。所有构建都只在本机和容器里进行，不会向连接的平板写入。

## 环境要求

| 目标 | 构建机 | 容器权限 |
| --- | --- | --- |
| UEFI 镜像、Android 引导助手模块 | x86_64 或 ARM64 的 Linux，装有 Docker | 普通容器 |
| 完整系统镜像（Debian GNOME / KDE、Nyarch GNOME、SteamOS） | ARM64 的 Linux，装有 Docker | `--privileged`，用于 chroot 和挂载 |

GitHub Actions 使用 `ubuntu-26.04` 运行主机检查，用 `ubuntu-26.04-arm`
构建固件、内核和发行版镜像。实际编译依赖来自下面的 Debian 构建容器，
与 GitHub runner 的 Ubuntu 版本分开管理。

## 取得源码和主机检查

```sh
./build.sh sources     # 取得固定版本的上游子模块，不递归下载测试数据
./build.sh check       # 主机检查，不需要平板
```

上游源码放在 `upstream/`，固定在指定提交。本地修改在 `patches/`、`uefi/`、`linux/`、`tools/`。构建在 `build/` 下的副本里应用补丁，不要改构建副本。
只构建某一部分时，可用 `./build.sh sources --group firmware`、`--group kernel`
或 `--group devices` 获取该阶段的子模块。移植 Windows 时用 `./build.sh sources --group windows`，
可取得小太阳的 Piano 平台、一加 Ace 5 Pro 的 UEFI 和整套公开 Windows 驱动源码。
各组共用同一份源码锁，不拉取其他组或上游测试数据。

## 构建容器

基础镜像记录在 `config/build-container.json`，定义是 `containers/Dockerfile`。Debian 基础镜像优先从 ECR Public 下载，元数据下载失败时改用 Docker Hub，两个来源固定同一个 OCI 摘要。

```sh
python3 tools/build_container.py     # 生成镜像 sunuefi-builder
```

APT 包版本会被记录，但构建结果不保证逐字节一致。

## UEFI 和引导助手

单独构建固件和引导助手时，先运行 `./build.sh sources --group firmware`。

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

## Windows ARM64 显卡驱动

`./build.sh windows graphics-umd --component all` 用固定版本的 Ace 5 Pro
源码构建配套的 ARM64 Direct3D 前端和 Mesa Turnip Vulkan 驱动。产物在
`artifacts/windows/drivers/adreno/Release/`。前者把绘图交给后者，两者都
需要平板上实际工作的 WDDM 内核驱动。只改一端时可以用 `--component direct3d`
或 `--component vulkan` 单独构建。

先运行 `./build.sh sources --group windows`。构建需要 LLVM（`clang-cl`、`lld-link`）、
Ninja、Bison、Flex，以及带 ARM64 库的 Visual C++、Windows SDK 和 WDK。
Turnip 的 Meson 和 Python 构建依赖可用
`python -m pip install -r windows/drivers/adreno/requirements-build.txt` 安装；
还需 `glslangValidator` 和能处理 ARM64 Windows 资源的编译器（用 `--windres` 指定）。
在配置好这些工具的 Visual Studio 开发者终端运行上述命令即可；
在 Linux 上或 SDK 与 WDK 分开安装时，显式传入相应目录：

```sh
./build.sh windows graphics-umd --component all --vc-tools "$VC_TOOLS" \
  --kit-include "$WDK_INCLUDE" --kit-include "$SDK_INCLUDE"
```

`--vc-tools` 指向 `VC/Tools/MSVC/<版本>`，`--kit-include` 指向各工具包的
`Include/<版本>`。如果 ARM64 库不在对应的 `Lib/<版本>` 下，用 `--kit-lib`
指定实际的 `um/arm64`、`ucrt/arm64` 目录。首次构建下载并校验 Mesa 头文件和
vkd3d 着色器编译器的源码归档，后续可复用 `--source-cache` 中的文件。

## Windows ARM64 安装介质

在 Linux 构建机上安装 `7z` 和 `wimlib-imagex`，传入自己的微软 ARM64 安装 ISO：

```sh
./build.sh windows media --iso /path/to/Windows11_arm64.iso
```

输出位于 `artifacts/windows/media/`。`esp/` 是应合并到 FAT32 启动分区的文件；
从 `EFI/Microsoft/Boot/bootmgfw.efi` 启动 WinPE，不覆盖已有的 `EFI/BOOT/BOOTAA64.EFI`
或更改启动顺序。`ntfs/` 是应放在 NTFS 卷上的 Windows Setup 安装源，其中
`sources/install.wim` 可大于 FAT32 的单文件上限。它不是已安装的 Windows 系统。
`media.json` 列出实际映像索引、版本和文件 SHA-256；构建不访问设备。

调试首次启动时加 `--probe`，输出到 `artifacts/windows/winpe/`。这份介质只启动
WinPE，不打开安装程序。启动脚本初始化设备，找到带 `SunUEFI/WinPE/probe.tag`
的卷后，在同目录记录本次启动、磁盘卷和设备，再重启。`memory.txt`
是 Windows 内核识别的物理内存地址段，不是固件报告的标称容量。若 Windows
没有识别带标记的卷，脚本仍请求重启，不写其他卷。普通安装介质不包含这个探测脚本。

需要额外的设备驱动时，构建介质可加 `--drivers /path/to/arm64-packages`，
适用于 WinPE 和完整安装。传入已解包的 ARM64 驱动目录，以供应商原来的布局保留
INF、签名目录（CAT）及其引用的所有文件。安装器把驱动放在 WinPE 内存盘里；
即使内置存储尚未被识别，也能先用 PnPUtil 将驱动加入 Driver Store，再初始化设备。
诊断介质将结果写到启动卷 `SunUEFI/WinPE/drivers.log`。完整安装则先展开 Windows，
再用 DISM 将同一批驱动加入新系统；启动卷 `SunUEFI/Deployment/` 里的 `drivers.log`、
`install.log` 分别记录两步结果。驱动签名和设备匹配均由 Windows 处理。
没有对应硬件的包可以预装，不代表该设备已工作。不指定驱动目录时普通介质保留原版 WIM。

构建项目自己的驱动包时，在装有 WDK 的 Windows 开发者终端运行
`./build.sh windows drivers --catalogs`。WDK 的 Inf2Cat 检查每个带 INF 的 ARM64 包，
生成列出包内文件哈希的 `.cat`，放在 `artifacts/windows/drivers/<项目>/Release/`。
新生成的目录文件没有签名；按 Windows 驱动签名流程签署后再交给安装介质。

在已创建独立分区的平板上，从有 root 的 Android 连接 ADB。Linux 主机还需要
Android platform-tools、`dosfstools`、`mtools`，完整安装另需 `ntfs-3g`、
`img2simg` 和 `wimlib-imagex`。先查看分区和介质选择：

```sh
./build.sh install windows --media artifacts/windows/winpe --esp-size-mib 2048
```

不加 `--execute` 时只读取设备。已有足够大的 ESP 可省略 `--esp-size-mib`；
调整容量时只移动相邻 ESP 和 `sunuefi_root` 的公共边界，保留两者的分区 GUID、
ESP 卷标、卷 ID 和其他系统的引导文件。WinPE 和正式 Windows 使用同一处微软引导目录；
把 WinPE 写入该 ESP 会替换已有的 Windows 启动配置。**移动边界会丢失当前 `sunuefi_root` 的系统和数据，
即使只安装 WinPE。** 查看结果后在同一条命令末尾加 `--execute` 执行。
保持 Android 作为测试期间的正常重启落点。

完整安装用 `--media artifacts/windows/media` 代替 WinPE 目录；这会把独立系统卷
改成 NTFS 并覆盖其内容。`--edition Professional` 是默认设置，也可指定
`media.json` 里的其他版本或索引。电脑写入并读回两个卷后，在平板引导菜单
选择“启动 Windows”（`\EFI\Microsoft\Boot\bootmgfw.efi`）。WinPE 启动后，DISM 只向此次标记的
NTFS 系统卷展开选定映像，BCDBoot 生成的微软启动文件放入共享 ESP，不
覆盖 `EFI/BOOT/BOOTAA64.EFI` 或重排固件启动顺序。WinPE 不会给整盘重新分区。
进度与失败原因在 ESP 的 `SunUEFI/Deployment/install.log`。如果写 ESP 时连接中断，
提示中的本地 FAT 镜像保留了原有引导文件；重新刷到 `sunuefi_esp` 即可恢复。
WinPE 重启后仍遵从原来保存的默认系统；再次选择“启动 Windows”即可进入
刚部署的 Windows，不会重新运行安装步骤。正式 Windows 首次启动时可能自行注册
Windows Boot Manager 并将它排在固件启动顺序的首位。其他启动项仍在，
需要时可在固件或操作系统里重新选择默认项。

## 完整系统

各目标共用同一份内核和 UEFI，各自构建根系统：

| 目标 | 根系统 | 构建目录 |
| --- | --- | --- |
| `debian-gnome` | Debian sid + GNOME | `build/distros/release` |
| `debian-kde` | Debian sid + KDE Plasma | `build/distros/release` |
| `debian-denial` | Debian sid + Denial | `build/distros/release` |
| `nyarch-gnome` | Arch Linux ARM + GNOME（Nyarch） | `build/distros/nyarch-gnome-piano` |
| `arch-denial` | Arch Linux ARM + Denial | `build/distros/arch-denial-piano` |
| `steamos` | Valve ARM64 SteamOS | `build/distros/steamos-piano` |

[发布配置](../../config/release.json)选择内核版本和[补丁系列](../../patches/linux/README.md)，
其中包含硬件适配、Gunyah 和 CachyOS BORE 调度补丁。实际使用的提交记录在
`build/release/source-manifest.json`，内核和模块输出到 `artifacts/kernels/release/`。
具体改动见[内核与调度配置](linux/kernel.md)。

重建内核时，旧的产物清单先作废，编译完成后才原子发布新的，避免旧 Image 配新模块。

### Debian

```sh
./build.sh linux
./build.sh release-rootfs                    # 也可选 --desktop kde 或 --desktop denial
./build.sh package --root-size-mib 8192
python3 tools/package_sparse_images.py --bundle artifacts/release --output artifacts/fastboot-sparse
```

已有根系统时，运行 `./build.sh esp` 只生成启动镜像，不重新导出 root 镜像。
启动镜像默认使用 BLS Type #1：内核、initramfs 和设备树（DTB）分开放在
`/linux/<release>/`，启动项放在 `/loader/entries/`。
SimpleInit 的“启动 Linux”进入 systemd-boot，菜单停留三秒，可以选择保留的旧内核。
加 `./build.sh esp --boot-layout uki` 可改为将这些文件和启动参数封装为
`/EFI/Linux/linux-<release>.efi`（UKI）。
UKI 打包使用 systemd 的 `ukify`，stub 来自 `config/release.json` 固定并校验的
Debian ARM64 `systemd-boot-efi` 包；默认 BLS 构建不需要 ukify 或 stub。
同一个软件包还提供 ARM64 systemd-boot，安装在 ESP 的
`EFI/systemd/systemd-bootaa64.efi` 和标准后备路径 `EFI/BOOT/BOOTAA64.EFI`。
单独提取的文件可通过 `--stub`、`--systemd-boot` 指定，`--ukify` 指定打包工具。

initramfs 的 BusyBox、分区识别工具和依赖库也由 `config/release.json` 固定并校验，
在临时目录解包，不从各发行版根系统取库。内核模块和键盘固件使用共享产品的同一份输入，
因此同次构建的系统可以使用同一个启动镜像。

基础系统已经构建过时，用 `./build.sh release-rootfs --resume` 更新内核、硬件包和配置，不重建桌面根。
需要重编的覆盖包沿用 CI 的独立 sid 构建根。续建保留桌面根的 SunUEFI 软件源，覆盖包编译只使用 Debian 官方源。
`./build.sh rootfs --distro ID --desktop ID --plan` 会列出发行版和桌面组合的计划，上表是成品构建目标。

内核和桌面包的并行度默认按 CPU 配额与可用内存选择，可用 `--jobs` 覆盖。
桌面包共用 `build/ccache`（或 `CCACHE_DIR`），FFmpeg、Mutter、GSD 和 GDM
会先校验并复用完整运行包。FFmpeg 构建中断后加 `--resume`，保留同一源码的对象文件。
x86 主机用 `./build.sh ffmpeg --sysroot 已完成的sid_ARM64根目录` 构建：编译器、链接器
和 make 在宿主 CPU 原生运行，头文件与库来自 ARM64 sysroot；QEMU 仅执行目标程序。
其他桌面包的 `--sysroot`／`--rootfs` 入口使用相同的原生编译工具。
宿主需安装 Clang、LLVM、AArch64 binutils、patchelf 和 ccache；Ninja 项目安装宿主 Ninja。
交叉编译使用 GNU 汇编器与链接器，保留 Debian 的符号版本表和调试信息处理。
产品 CI 使用原生 ARM64 构建机，缓存与源码、补丁匹配的 Debian 运行包。

Debian 的 Mesa、FFmpeg、sensors 在 sid 根目录的隔离副本中构建，不安装宿主容器的库或混用 trixie-backports。`--sysroot` 不修改输入根目录；包版本、源码归档、补丁和二进制哈希记录在各输出目录。三个产品共用 `config/mesa.json` 固定的官方 Mesa 27 开发源码；Debian 保留 sid 原始完整配方，Nyarch 使用 Arch Linux ARM 的完整配方。官方源码已包含 830v1 和 DirectX-Headers 修正，旧26.2回移植已删除。SteamOS 的 renderonly 高度修正仍单独保留，因为该官方提交只对齐缓冲宽度，不保证非4行对齐模式的尾部空间。

已有 sid ARM64 根目录时，可以单独重建这三组包：

```sh
sudo ./build.sh mesa --sysroot build/distros/debian-sid/rootfs
sudo ./build.sh sensors --sysroot build/distros/debian-sid/rootfs
sudo ./build.sh ffmpeg --sysroot build/distros/debian-sid/rootfs
```

### 共用构建与缓存

Build products 先准备一次 ARM64 构建容器，再并行构建 UEFI 和 Android 模块、内核包、设备配置和相机包。覆盖包随后按 Debian、Arch Linux ARM、SteamOS 分别构建；同一发行版的不同桌面共用硬件覆盖包。发布阶段签名并上传 R2，从公开域名验签和核对索引后，各系统才从对应的软件源安装这一批包。中间产物不增加用户下载项，各阶段的日志仍合并进 `logs.zip`。

Debian 覆盖包在由官方签名 sid 源引导的 ARM64 构建根中编译。各包按原生配方安装构建依赖和运行依赖。完整桌面、文字编辑器和 Chromium 由系统镜像阶段安装；这些应用的依赖不影响覆盖包构建。

KDE 镜像明确安装 `plasma-desktop` 和 `upower`。最小化安装不拉取推荐包，
仅安装 `task-kde-desktop` 不会带入 Plasma 的桌面外壳，也不会保证电池服务可用。
镜像检查同时核对已安装的包和桌面外壳的实际文件。

同一分支的产品运行按批排队，只保留最新的排队任务。pacman 同步索引只列出每个包的
最新版本，所以已经开始的任务要等镜像组装结束，下一批才会替换公开索引。
构建任一 Arch 镜像时，两套发行版的 SensorProxy 包一起检查和发布。共用的
`piano-sensors` 需要这两套各自支持 SSC 的包；发布先更新 SteamOS 增量仓，再更新共用仓。
如果从还会直接复制传感器服务的构建目录续建，安装器会拒绝覆盖无主文件；
请指定新的构建输出目录。
同批的各发行版仍并行构建。
镜像组装读取发布任务验签成功后传来的包清单；重跑重新构建了包时，也不会读回旧清单。

同一轮任务通过 GitHub Actions 产物传递构建器、共享固件和内核、设备包及
覆盖包。不依赖跨轮缓存保留这些输入；下载后继续核对原构建清单和归档摘要。
成功后删除本轮中间归档，下载区只留产品和日志；失败的归档保留七天供重跑。

完成缓存只在构建、源码和输出校验成功后发布。缓存键包含构建器、绑定源码、补丁、配置、架构、工作区路径及实际容器镜像 ID。内核恢复保留 Image、完整模块集及外部模块构建需要的配置、符号表和头文件；原生内核入口仍核对 SOURCE、编译器、配置、模块 hash 和 vermagic。root job 检查不匹配时停止，不自行重新编译或混用旧产物。UEFI 缓存保留原有构建标记、同核心模块身份与完整文件摘要，不把未完成的构建标为完成。

内核只绑定 `config/release.json` 的 `kernel` 子对象，按规范化 JSON 记录内容与 SHA256；修改 sensors、Mesa、登录或其他根系统配置不使内核缓存失效，内核配置、源码与工具链变化仍会失效。旧内核清单只保存整份配置文件摘要，无法还原当时的子对象，需要正常构建一次生成新记录；不会改写旧清单来跳过构建。

桌面覆盖包的选择不参与内核缓存。内核和其他原生包共用归档读取与下载校验；修改这部分代码仍会更新内核缓存键。

固定 LLVM 归档、固件中间对象及 ccache 分开缓存，避免每次下载工具链或因小改动完全丢失对象缓存。只保存 LLVM 压缩归档，不把展开后十余 GiB 的目录占满 Actions cache。构建容器按 Dockerfile、固定基础镜像及构建器区分，载入时核对归档摘要和 OCI 镜像 ID。每种共享缓存只有一个写入 job，本地入口通过文件锁串行更新并原子发布缓存记录。
LLVM 缓存缺失时重新下载同一官方归档，并核对固定的 SHA256。

本地需要连续构建多个发行版时，在已准备好的构建环境里使用同一入口：

```sh
python3 tools/build/shared_products.py ensure --kind firmware
python3 tools/build/shared_products.py ensure --kind kernel
# 后续只校验共用产物；输入不匹配则停止
python3 tools/build/shared_products.py verify --kind firmware
python3 tools/build/shared_products.py verify --kind kernel
```

这些命令只由 CI 或维护者的本地共用构建调用，不改变 rootfs 的安装过程。成品缓存只收录已完成的覆盖包档案与清单，不保存半装好的构建根。复用前仍准备新的发行版基底，核对实际构建容器、源码与配方、所用内核接口、已安装包、仓库配置和索引；Nyarch 另核对应用实际构建的 Git 提交。任一输入不匹配就重编，不把滚动仓库锁在旧索引上。

覆盖包构建更新了编译缓存时，即使随后有包编译失败，CI 也会保存 ccache。重跑只复用输入匹配的编译结果；未完成的包组不能发布。直接复用完整包组时，不重复上传未变的编译缓存。

Debian 核对已签名的 sid `Packages` 与 `Sources`，子构建器共用已选定的索引；Arch 系用各自 pacman 配置下实际的数据库，安装包时仍按发行版签名策略校验。缓存命中后逐个核对包名、版本、架构和 SHA256。完整 root 镜像不存入构建缓存。首次编译、仓库刷新、根系统安装和镜像导出仍需要时间。
只改主机检查入口不会使产品缓存失效；UEFI 的变量处理脚本只影响固件缓存。

CI 实际重编覆盖包时，使用该次运行和重试编号增加原生包修订号：Debian 在配方版本后加 `+sunuefi.<运行编号>.<重试编号>`，Arch 使用 `pkgrel` 的数字子修订。属于同一源包的多个二进制包保持同一版本。命中缓存或直接使用 Debian 官方签名的 FastRPC 包时，保留原版本和原文件；本地构建默认仍使用配方版本。

pacman 的发布清单可包含完整的应用和媒体拆包。组镜像时只装桌面默认包、
它们的依赖，以及系统已安装的对应拆包；例如不会同时安装两种 JACK 实现。
镜像完成后回读这一批实际装入的版本。Arch Linux ARM 与 SteamOS 的
相机包共同提交发布。首次换用标准 V4L2 模块时，先从共用索引撤下旧插件并验签回读，
再切换并验签 SteamOS 索引，最后发布新的共用索引。旧包仍可用于回退。
SteamOS 为共用源中的媒体运行包提供同名 Valve 构建，只有不依赖运行库的
`libcamera-docs` 共用。

只改 `tools/repo/` 下的软件源发布逻辑时，UEFI 和输入不变的覆盖包可以复用缓存；
发布前仍用新的检查代码验包。SteamOS 更新 Valve 基础包后，先确认相机包仍使用
Valve 的包数据库，再开始编译覆盖包。

`./build.sh camera-package` 在构建容器中生成 `piano-camera` 的两种 ARM64 原生包。
相机包使用发行版的用户空间接口头文件和静态库，不依赖某一次内核编译。
缓存按相机源码、依赖补丁、配方、版本和构建器区分；只更新内核时复用原包。
发布版本和最低内核元包版本在 `config/release.json` 的 `camera_package` 中；
修改已经发布的包内容时提升 `revision`，不要用同一版本覆盖不同文件。

本地和 GitHub 的完整构建走同一个 `release-rootfs` 入口。默认系统包含中文界面、拼音输入法、Chromium、GNOME 动画、蓝牙地址自动初始化和触控笔组件。根系统生成后会检查这些软件包和必需文件，结果记录在 `manifest.json` 的 `default_system`，缺少任何一项构建就失败。

Debian 只构建 sid，不混用 stable 或 backports。`release-rootfs` 从独立的 sid 构建根编译 sensors、FFmpeg 和桌面包，再将对应的原生包装入镜像。CI 保留编译缓存，但按 sid 配置和构建器区分缓存，并由软件包构建器检查所选源码版本后复用。

Debian KDE 由 KWin 启动 Fcitx。Plasma 自带的设置门户配置优先用 KDE，
缺少的设置由 `xdg-desktop-portal-gtk` 补齐。GTK 应用可自行选择 Wayland
输入法或 XWayland 下的 Fcitx 插件，无需全局设置 `GTK_IM_MODULE`。

Debian 镜像用 `update-alternatives` 选择 `wireless-regdb` 包自带的上游签名版本。
数据库与签名一起切换，普通 APT 更新会保留该选择；共用内核仍用上游公钥验签。
已有 Debian 安装可运行
`sudo update-alternatives --set regulatory.db /lib/firmware/regulatory.db-upstream`，
重启后内核会重新读取数据库。

### Arch Linux ARM 和 SteamOS

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

Arch Denial 使用 `./build.sh arch-denial-rootfs`，路径换成 `arch-denial-piano`；
SteamOS 使用 `./build.sh steamos-rootfs`，路径换成 `steamos-piano`。Nyarch 从
Arch Linux ARM 基础系统开始，通过签名仓库安装软件包，再构建 Nyarch 应用和
Piano 的原生硬件包，源码和扩展选择在 `config/nyarch.json`。

构建 Nyarch 和 Arch Denial 时，从 `config/nyarch.json` 指定的同一个 Arch Linux ARM
镜像取索引和软件包，避免从一个镜像拿索引，再去另一个镜像找对应的包。
安装后的官方镜像列表不变。SteamOS 的构建细节见 [SteamOS ARM64](linux/steamos.md)。

两种 Denial 产品镜像从 SunUEFI 签名源安装接入 SensorProxy 的合成器。
Debian 从 [Denial 签名源](https://github.com/denialwm/denial/blob/main/docs/INSTALL.md#repository-trust-paths)
获取匹配的原版 Flutter Engine，系统依赖仍用 sid。Arch 的 `[sunuefi]`
在同一份索引里提供合成器、可选插件管理器和字节不变的原版 Engine；
更新这组包时按同一版核对，避免新版 Engine 先于合成器升级。
SDDM 加载 Denial 原版 Wayland 会话，默认使用触控布局。Piano 的显示控制器
和 Adreno GPU 是不同的 DRM 设备；用 Denial 自带的 `DENIAL_RENDER_DEVICE`
指向 GPU 的稳定 udev 路径，内屏输出仍由显示控制器负责。相机和其他硬件继续
用 Piano 的标准驱动和发行版软件包。独立的本地 rootfs 构建仍可使用上游包。

两版默认使用 Thunar、foot 和 Chromium，随发行版安装文件传输、拼音、
剪贴板、mpv 和 imv。文件关联用标准 XDG 设置，Thunar 通过 GVfs 访问
U 盘、手机与 SMB。权限确认、网络、蓝牙及音量继续由 Denial 提供；
文件选择和屏幕共享使用随 Denial 配置的 GTK 与 wlr 门户。

开发时可用 `./build.sh denial --test` 测试
[SensorProxy 补丁](../../patches/denial/0.6.8/series.json)。
在 ARM64 构建环境运行 `./build.sh denial --release`，或在 x86-64 上用
`./build.sh denial --arch-sysroot "$ARCH_ARM64_ROOT"` 交叉编译。
这些命令从同一签名标签构建，补丁优先使用标准加速度计接口，内屏熄灭
后释放请求。产品任务只编译一次 ARM64 合成器；缓存键绑定源码、补丁、
Rust 和实际安装的原生依赖，复用前核对文件摘要和源码记录。
`--overlay-target` 从同版签名发布取得原包和校验清单，为对应产品生成原生包：

```sh
./build.sh denial --overlay-target debian-denial \
  --package-output artifacts/packages/overlays/debian-denial
./build.sh denial --overlay-target arch-denial \
  --package-output artifacts/packages/overlays/arch-denial
```

发布使用现有 APT 和 pacman 索引。只有 Denial 镜像读取对应包清单，
先通过原生包管理器验签、比对归档摘要，再安装。产品任务重打
`denial` 和 `denial-plugin-manager`，Arch 的 Engine 不重打。

Droidloom 使用独立的原生包。`./build.sh droidloom-package --help` 列出
Debian 和 Arch 的构建参数：从固定源码为目标发行版编译宿主程序，从上游
Android 构建目录或固定摘要的 ARM64 归档取得镜像。`runtime`、`image` 和
`base` 通过原生依赖保证版本配套，安装时不启动容器，也不改原厂 Android。
首次配置完成后，从桌面点 Android 应用会按需启动 Droidloom。

已有隔离的 ARM64 pacman 构建根目录时，可单独构建并安装完整 Mesa 包；此操作会修改指定的构建根目录，不要直接使用已交付的产品根目录：

```sh
sudo ./build.sh mesa build/mesa-nyarch --format pacman --rootfs build/distros/nyarch-builder/rootfs --jobs 8
sudo ./build.sh mesa build/mesa-steamos --format pacman --rootfs build/distros/steamos-builder/rootfs --steamos --jobs 8
```

中断后使用相同参数加 `--resume`，保留已配置的源码和对象。SteamOS 构建保留 `deckard-mesa-linux-aarch64` 包名及 renderonly 高度补丁；Nyarch 保留 Arch Linux ARM 的原生拆分包。

两种产品构建都会检查 Mesa 源码、完整驱动选项和 ARM64 包。CI 分别缓存原生包；复用前核对来源、配方与包摘要，安装后重新检查 ELF 和目标系统的依赖，避免把不匹配的滚动发行版缓存带入镜像。

各产品由 APT 或 pacman 安装键盘套、显示校准、触屏和指纹固件包，组装时核对包归属、
文件大小和 SHA256。`piano-firmware-keyboard` 和 `piano-firmware-touch` 随共用
设备包发布到 APT 和 `sunuefi`，SteamOS 也从共用仓安装。显示包随 CI 共用设备
产物提供，指纹包在组装时从固定固件清单构建；这两个包不上传软件源。其余设备
固件仍按 `vendor/firmware/` 下的组件清单校验后安装。

### 单独构建的软件包

仅在需要指纹诊断时构建、安装 `piano-fingerprint-tools`。使用当前
内核导出的头文件目录（其下有 `include/linux/tee.h`）：

```sh
./build.sh bsp package --fingerprint-tools build/runtime/uapi \
  --target debian --format deb --output build/fingerprint-tools/debian
./build.sh bsp package --fingerprint-tools build/runtime/uapi \
  --target arch --format pacman --output build/fingerprint-tools/pacman
```

Debian 用 `apt install ./路径.deb`，Arch 和 SteamOS 用
`pacman -U ./路径.pkg.tar.zst`。运行 `piano-fingerprint-ctl --help`
不访问传感器；日常指纹功能不需要这个包。新版设备配置包会清理由旧镜像
复制且未被修改的旧工具，保留管理员自己的副本和已有的指纹登记。

CPU 型号由 `linux/dts/piano-cpu-model.dtso` 给八个 CPU 节点加标准 `model` 属性，内核补丁把它输出为 `/proc/cpuinfo` 的 `model name`，让 GNOME 显示 Snapdragon 8 Elite。MIDR、核心拓扑、时钟和板型号保持原值。

`./build.sh gsd --sysroot 已完成的基础根目录` 在独立副本里编译带 Piano 自动亮度策略的 gnome-settings-daemon，使用原厂 lux 阈值和延迟，不修改传感器读数，由 APT 安装。默认的 `release-rootfs` 会自动完成这一步，策略只匹配 `xiaomi,piano` 板级标识。

`./build.sh mutter` 从签名 sid 索引选择 Mutter 源码，与 Nyarch 共用空 Gamma 曲线恢复和已取消触摸 grab 两项修复；无 EDID 的标准 sRGB fallback 已由发行版提供，不再重复打补丁。标准颜色服务和用户配置保持不变。来源与补丁见 [Mutter 适配说明](../../linux/desktops/gnome/patches/mutter/README.md)。
单独构建时使用了其他输出目录，可通过 `release-rootfs --mutter-dir 输出目录/runtime` 选择；组装前会检查源码、补丁和软件包摘要。

## 软件包与更新标准化

硬件支持应能安装到干净的受支持发行版中，再通过发行版包管理器升级、卸载和
回退。产品镜像是这些软件包和首次安装默认设置的组合，不应成为硬件适配
唯一能生效的环境。下面区分已经使用的原生包和仍由构建脚本复制的文件；
未完成的实施工作见[发布要求中的开发清单](release.md#未完成的开发工作)。

| 内容 | 现有安装方式 | 标准化方向 |
| --- | --- | --- |
| Linux 内核与模块 | Debian 的 `bindeb-pkg` 和 Arch 的 `pacman-pkg` 原生包 | 随包安装并维护匹配的模块，由元包选择最新版本 |
| 设备树 | 组合板级 DTB、overlay 和绑定转换，按内核版本打包 | 生成启动文件时选取同版文件，逐步收敛到内核源码中的板级设备树 |
| Linux 设备固件 | 键盘套、显示校准、触屏和指纹通过[原生包](../../linux/bsp/README.md#设备固件包)安装，其他固件仍由组装脚本安装 | 发行版提供通用固件，设备包补齐板级固件 |
| ALSA UCM、udev、笔校准及音频拓扑 | 成品安装 `piano-device-config` 和 `piano-audio-topology` | 开源拓扑随独立包更新，剩余硬件运行文件继续按组件打包 |
| Mesa、FFmpeg | 已有 Debian 和 pacman 原生包 | 保留目标发行版完整配方、功能和 ABI，按软件源正常更新 |
| 传感器 | Debian 提供六个原生包；Nyarch 与 SteamOS 共用 `piano-sensors`，FastRPC、libssc 和 SensorProxy 按各自的库接口打包 | 继续通过标准 SensorProxy 接口提供照度和姿态 |
| PipeWire | Debian、Arch Linux ARM 和 SteamOS 分别重建原生同版包组 | 保留发行版的依赖关系，升级到包含修复的上游版本后删除回移 |
| 指纹 | libfprint／fprintd 使用目标发行版包，设备权限和模板目录配置归 `piano-device-config`，固件独立打包 | 诊断工具独立打包，标准认证结果仍取自 libfprint／fprintd |
| 相机 | 两种包管理器安装 `piano-camera`；`v4l2loopback` 随匹配内核安装 | 标准 libcamera 路径接管后移除过渡服务 |
| 笔及诊断工具 | `piano-device-config` 安装笔的 BlueZ 服务和依赖；诊断工具仍由构建脚本复制 | 开发工具独立打包，不成为笔输入的依赖 |
| GNOME／KDE／SteamOS 组件 | Mutter、GDM、GSD、libwacom、Gamescope 等已有原生包，默认设置和会话文件仍有 overlay | 通用修复维护一份补丁，设备配置与产品默认值分开，不覆盖用户设置 |

Linux 设备固件包括 ADSP、CDSP、Goodix 签名程序及模型、ath12k、蓝牙、GPU、
VPU、内屏、键盘和触屏文件，来源由 `config/release.json` 和组件清单记录。
原厂固件的许可与再分发条件需逐项核对，不能由本项目源码许可覆盖。
通用 Adreno 固件由 Debian 的 `firmware-qcom-soc` 和 Arch Linux ARM、Valve
SteamOS 的 `linux-firmware-qcom` 提供。原厂 AQE、SQE 与这些公开版本不是同一份
文件，不宜只按文件名替换；板级 ZAP 也不同于 Qualcomm 参考板的签名版本。
SunUEFI 固件和 Android 引导助手使用另一条更新链；安装 Linux 内核或硬件包
不应顺带覆盖 Android BOOT。

### 内核软件包

Debian 优先复用选定内核的 `make bindeb-pkg`，提供按 ABI 版本命名的
`linux-image-<release>`、`linux-headers-<release>`，再由元包依赖最新版本。
Arch 系使用内核源码自带的 `pacman-pkg`、`PKGBUILD`／`makepkg`，
提供按 release 命名的内核和 headers 包，`linux-piano` 与
`linux-piano-headers` 元包选择最新版，让旧内核及模块可以并存；不得将
设备内核声明为可满足发行版原版 `linux` 的二进制模块依赖。
共用源码、补丁、配置和版本定义，不把 `.deb` 当作 pacman 包安装。

内核 release 必须区分会影响模块兼容性的修改，避免不同驱动占用同一个
`/usr/lib/modules/<release>/`。模块与内核的构建配置、导出符号必须匹配，
不能只比较 `vermagic`。headers 包需包含外部模块构建所需的生成头文件、
配置、脚本和 `Module.symvers`；`make headers_install` 导出的用户空间
接口头文件不能替代它，也不需要因安装设备内核就替换发行版 `linux-libc-dev`。
交叉构建时还需检查 headers 内附工具能在目标机运行。

```sh
./build.sh linux-package --format debian --output artifacts/packages/kernel/debian
./build.sh linux-package --format pacman --output artifacts/packages/kernel/pacman
```

CI 在 ARM64 构建器打包。交叉构建需要 ARM64 GNU 工具链及 libc；
内核和包构建共用同一套编译选项。加 `--plan` 可只检查命令。
headers 包校验 `Module.symvers`、生成头文件和构建工具架构；只发布内核和
外部模块 headers，不发布随 `bindeb-pkg` 生成的 `linux-libc-dev`。
内核 release 包含源码、配置和编译工具的摘要及 `config/release.json` 中
固定的 `abi_epoch`，不使用项目最新提交的时间。同一组输入清空缓存重建时，
模块目录不变。发布新的内核二进制或 headers 时，把 `abi_epoch` 更新为
比上一版大的 Unix 时间，并将 `package_epoch` 推进到不早于它的值。
`linux-piano-dtb-<release>` 随同发布完整板级 DTB；`piano-boot-config`
连接 Debian 内核钩子或 pacman 事务钩子、发行版 initramfs 工具和 `kernel-install`。
只更新板级 DTB、元包或引导配置时，只推进 `package_epoch`。软件包版本按
`abi_epoch`、`package_epoch` 的顺序比较，不改变现有内核 ABI；升级已安装
内核的设备树时，包管理器也会刷新启动文件。
只修改 Debian 内核打包规则时，递增 Debian 包修订号，保持内核 ABI 不变。
产品系统通过 `/etc/kernel/entry-token` 沿用初装启动镜像的 `linux` 标识；
在独立安装的发行版中装包时，由 `kernel-install` 选择该系统的标识。

DTB 与内核一同构建和按版本安装，避免旧内核使用被覆盖的新设备树。
长期把板级描述收敛到内核的 `arch/arm64/boot/dts/qcom/`，减少外部转换；
同次启动的动态内存和固件资源仍由 UEFI 通过标准设备树修正协议提供。

### 从装包到启动项

正常升级应由以下流程完成，而不是重新刷写整个 ESP：

```text
APT／pacman 安装内核、模块、匹配 DTB 和必要固件
  → 处理外部模块及 depmod
  → 发行版 initramfs 工具生成对应版本的 initramfs
  → kernel-install 按所选布局安装内核、initramfs 和 DTB
  → systemd-boot 发现 BLS 启动项或 UKI
```

Debian 复用 initramfs-tools 和 systemd-boot 提供的内核、initramfs 钩子；
`55-initrd.install` 将生成的 initramfs 交给 systemd。直接安装版本化内核包时，
APT 也会先配置匹配的设备树与 `piano-boot-config`，再生成启动文件。
Debian 可能在挂载根之前加载数字信号处理器（DSP）驱动。`piano-boot-config` 的
initramfs 钩子会带上已安装的 ADSP、CDSP 固件头和分段，升级引导配置也会触发
initramfs 重建。平板已确认两颗 DSP 在冷启动时自动运行，Type-C 和 USB 网络随之上线，
保留的旧启动项不受这次重建影响。

Arch 由 pacman 事务调用 `kernel-install`，复用官方 `50-mkinitcpio.install`，
并按 [ArchWiki 的 kernel-install 配置](https://wiki.archlinux.org/title/Kernel-install)
屏蔽 preset 模式的两条 mkinitcpio 事务钩子，避免重复生成。
固件、initramfs 工具及相关启动组件更新时，事务钩子重建所有已安装内核的启动文件。
SteamOS 的 Frame 存储分组、恢复、ADB 和固件钩子不适用这台平板；
Piano 的 mkinitcpio 配置只跳过这些钩子，保留系统及用户配置的其他钩子。

systemd-boot 本身由发行版提供。成品启用 systemd 的 `systemd-boot-update.service`，
用软件包里的 EFI 文件更新启动分区上的副本，不需要写固件变量。`piano-boot-config`
让该服务等已安排的启动分区挂载完成；未配置启动分区挂载时，不额外启动挂载任务。
SteamOS 用这个服务在下次开机同步 EFI 文件。Debian 的 `systemd-boot` 包还会在
APT 配置软件包时执行更新，可能补录固件启动项；目标 ESP 没有 systemd-boot 时，
Debian 包脚本可执行首次安装并接管默认 EFI 文件。

Nyarch、Arch Denial 和 SteamOS 把 ESP 直接挂在 `/boot`，内核、initramfs 和启动菜单在同一个分区。
Debian 把 ESP 挂在 `/boot/efi`，APT 管理的内核文件仍留在根文件系统的 `/boot`。
自己安装发行版时，也可以选择 `/efi` 等标准位置。用 `bootctl --print-esp-path`
查看 ESP，用 `bootctl --print-boot-path` 查看 `kernel-install` 写启动文件的位置。
没有独立的 XBOOTLDR 分区时，两者通常相同。

安装 Piano 内核时，`piano-boot-config` 在 initramfs／UKI 已生成、systemd 写入前
检查启动分区能否容纳新文件。在 FAT 上更新同一个入口时，只预留文件增长所需
的空间；安装另一个版本仍要容纳完整的新入口。如果空间不足，安装会先停止并提示
从包管理器卸载不用的旧内核，不会为了给新版本腾空间而删除已有入口。

默认 `/etc/kernel/install.conf` 为 `layout=bls`；需要 UKI 时改为 `layout=uki`。
Arch 系运行 `sudo kernel-install add-all`；Debian 的内核文件在 `/boot`，
运行 `sudo kernel-install add "$(uname -r)" "/boot/vmlinuz-$(uname -r)"`，
或为需要添加的其他版本明确指定 release 和内核文件。BLS 使用 systemd 的
`90-loaderentry.install`；UKI 使用 `60-ukify.install` 和 `90-uki-copy.install`。
`/etc/kernel/cmdline` 可以覆盖包内的默认启动参数，
`/etc/kernel/devicetree` 可以覆盖默认 DTB 选择；这些管理员配置优先于 `/usr/lib/kernel/`。
包管理器升级保留 `/etc/kernel/install.conf` 等可修改配置。
根设备可设为 `root=UUID=…` 或 `root=PARTUUID=…`，包内默认使用 `root=LABEL=PIANOROOT`，
不带首次安装 initramfs 使用的私有参数。

启动文件按安装标识及内核版本命名，菜单名称取自目标系统的 `os-release`；
仍可通过标准 UEFI 启动项改用其他引导程序。
共用的初装启动项不设排序键，升级后由发行版生成的入口优先；
仍可从菜单选择旧入口，也可改用自己的启动参数、名称和更新方式。

回退必须同时保留旧启动文件、模块和 DTB。Debian 和 Arch 的设备内核都按
release 使用不同包名，元包升级不会替换旧版本包；从 systemd-boot 菜单选择旧版即可。
相机 `v4l2loopback` 模块也按 release 并存，由内核元包安装匹配版本并交给
包管理器更新模块索引。
Arch 默认不生成可选的 `fallback` initramfs；它包含同一内核的更多模块，
不能替代保留旧版内核。
卸载旧版内核会移除该版本的启动项，不删除其他系统的启动项。
原生包要求 systemd 258 或更高版本。如果同一版本从 BLS 切换成 UKI，
卸载也会调用 systemd 的 BLS 插件清理旧启动项。
更新前应保持 ESP 正常挂载且空间充足；initramfs 生成失败时，先修复报错，
保留原内核与启动文件，不继续卸载旧版。

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
APT 先上传 SHA512 和 SHA256 的 `by-hash` 索引，再更新 `InRelease`。
发布后从公开域名核对两种地址，避免 APT 退回可能仍被缓存的 `Packages.gz`。
下次发布从这份签名索引继续。
产品构建先核对 pacman 新索引选中的包名、版本和 SHA-256，再上传索引。
如果索引已选中更新的版本，发布会停止；用高于已发布版本的包重新构建。
Arch 的 `libpipewire` 和 SteamOS 的 `pipewire` 负责替换各自的旧
`pipewire-v4l2-source`。旧插件从相应仓库的当前索引移除；
旧归档和已签名的索引仍可用于回退。
同一上游内核版本的新 ABI 必须推进 `abi_epoch`，避免摘要的字母顺序决定升级方向。
共享内核包完成后，CI 先与公网签名索引比对，再构建各发行版的软件包；
公网暂时无法验签时，发布步骤仍从 R2 核对实际索引。
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
同一分支的产品任务排队串行执行。后一次提交不会顶掉等待中的任务，也不会在前一轮镜像安装软件包时更新软件源索引。

| 下载名称 | 内容 |
| --- | --- |
| `SunUEFI-Module` | Android 引导助手模块 ZIP |
| `SunUEFI-UEFI-Fastboot` | 用于 `fastboot boot` 的独立 UEFI 镜像 |
| `SunUEFI-ESP` | 各发行版共用的稀疏 ESP 镜像 |
| `SunUEFI-Root-发行版` | 对应[构建目标](#完整系统)的稀疏 root 镜像 |
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
