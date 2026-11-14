# 补丁管理与应用顺序

固件改动通过 Git patch 和准备脚本共同完成，分别服务于不同上游的固定输入。本页记录补丁的目标、应用顺序和校验入口。

## 产品构建中的顺序

`tools/build_product.sh` 先建立 `build/firmware-workspace/` 副本，在副本中依次应用补丁，上游子模块保持干净：

| 顺序 | 步骤 | 目标 | 说明 |
| --- | --- | --- | --- |
| 1 | `tools/apply_firmware_patches.py` | `upstream/Mu-Silicium`、`simple-init` | 应用 `patches/firmware/series.json` 中登记的 Git 补丁 |
| 2 | `tools/prepare_product_handoff.py apply` | Mu Basecore DXE | 安装晚期 APP 身份检查和退出钩子 |
| 3 | `tools/prepare_product_pump.py apply` | Mu DXE Event、Silicium 库绑定 | 安装事件分发与产品 pump 支持 |
| 4 | `tools/prepare_product_ui.py apply` | Mu DisplayEngine、SetupBrowser、UiApp | 安装菜单导航与退出处理 |
| 5 | `tools/prepare_nv_runtime_guard.py apply` | Basecore RuntimeDxe Variable.c | 安装变量运行时保护（编译参数 `PIANO_NV_BOOT_ONLY=1`） |
| 6 | `tools/build_simpleinit.sh --product-gui-pump` | SimpleInit | 编译 SimpleInit 图形菜单 |
| 7 | `tools/prepare_product.py` | `pianoProductPkg` | 组装组件、早期内存校验，生成产品平台描述 |
| 8 | `tools/build_stage0.sh product` | Mu Basecore | 编译固件（自动应用 Mu `Resources/MuPatches/` 补丁） |

准备完成后，可单独校验各步骤是否到位（在工作区目录下运行）：

```sh
python3 build/firmware-workspace/tools/prepare_product_handoff.py verify
python3 build/firmware-workspace/tools/prepare_product_pump.py verify
python3 build/firmware-workspace/tools/prepare_product_ui.py verify
python3 build/firmware-workspace/tools/prepare_nv_runtime_guard.py verify
```

## Linux 补丁

Linux 内核补丁按内核版本分目录维护在 `patches/linux/`，通过 `patches/linux/7.3/series.json` 登记，在 `./build.sh linux` 阶段自动应用。补丁详情见[内核与调度配置](linux/kernel.md)。

平台、显示、输入与无线、媒体和音频的前五个主题包仍是组合下游移植，没有拆成可逐项提交的子系统系列。清单逐个记录替换它们所需的硬件能力与检查，这些包不是已被主线接收或可直接提交的系列。

`original_commit`、`original_parent` 和 `tree` 用于复现本地补丁链；上游状态以各条目的 `upstream` 为准。参考了公开补丁系列的本地改写仍可标为 `downstream`，参考链接不表示这份改写已经提交。来源元数据不替代补丁头的作者、签名和逐文件许可声明，也不补造缺失的签名。

## 软件包补丁与产品目标

软件包系列使用 `patches/<包>/<版本>/series.json`。`patches` 列表按顺序应用，条目的 `file` 相对该系列目录解析；FFmpeg 的现有 `series` / `path` 格式含义相同。同一修复适用于多个版本时，系列引用同一份文件，不再复制补丁。跨版本路径可以使用 `../`，但不能离开仓库。

`targets` 登记使用该版本的 CI 产品名，以 `tools/ci_build_targets.py` 中的 `TARGETS` 为准：固件和内核分别可选 `uefi`、`linux`，根系统使用四个发行版目标。PipeWire 的 Debian 两桌面使用 1.6.9，Nyarch 使用 1.6.11，SteamOS 使用 1.6.8。GTK 3.24.52、IBus 1.5.34、KWin 6.6.5 和 Mesa 27.0 的本地系列由 SteamOS 配方使用。Nyarch 的 Material You Colors 30 和 Blur my Shell 75、SteamOS 的 KScreen 6.6.5 和 Manager 26.4.1 也通过各自版本的系列选择补丁。

固件系列同时登记 `uefi` 和四个根系统目标，内核系列登记 `linux` 和四个根系统目标。F2FS 工具装入 Android 引导助手，不装进 Linux 根系统；四个根系统产品也会构建配套助手，因此其系列登记 `uefi` 和四个根系统目标。Gamescope 系列只由 `steamos` 使用。

已登记系列中的补丁必须有 `upstream`。共享文件的来源只需在一个引用处记录一次；一个系列的补丁确实共用来源与删除条件时，也可在系列顶层记录：

| 字段 | 含义 |
| --- | --- |
| `status` | `merged` 表示修复已在上游提交中，`submitted` 表示有实际提交入口，`downstream` 表示项目或其他下游修复 |
| `url` | 上游提交、提交入口或下游源码的 HTTPS 地址 |
| `commit` | 已知时填写完整上游提交号 |
| `drop_when` | 换到哪个包含修复的源码、需要通过什么功能检查后才能删除 |

`merged` 不表示正在使用的发行版已包含修复。例如 PipeWire 的五项 GStreamer 修复已在上游 master，但 1.6.8、1.6.9 和 1.6.11 的 release 源码仍需回移；GTK 3 维护分支已有工具原子修复，固定的 3.24.52 release 尚不包含它。删除前必须核对所选 release 的实际源码，而不是只比较版本号或提交日期。

### 版本选择与删除边界

| 系列或构建依赖 | 产品选择 | 删除或保留的依据 |
| --- | --- | --- |
| Mesa | 四个产品共用官方 27 开发源码；SteamOS 单独保留高度修正 | 不再依赖外部 `piano-mesa` 仓库、裁剪配方、830v1 芯片 ID 或 D3D12 回移补丁；SteamOS 的 renderonly 分配短缺仍存在 |
| GSD、GDM、Mutter | Debian sid 和 Nyarch 的实际 GNOME 51 版本 | 删除旧 GNOME 48／50 专用系列和重复补丁；保留不同产品实际使用的包版本，共享同一修复文件 |
| UPower | 各发行版原生包 | 已包含键盘背光插拔处理，删除旧 1.90.9 回移和专用构建器，由 GSD 刷新标准背光对象 |
| FFmpeg、传感器 | sid 原生配方，Arch 系按各自 ABI 构建 | 删除 FFmpeg 7 和 trixie-backports 专用来源；libssc 的向量接口只保留在有消费者的传感器系列中 |
| 指纹传输 | 自包含 Linux TEE 对象调用 | 删除 MinkIPC／quic-teec 的链接依赖与适配补丁；UIO 及本地 libfprint 后端仍有消费者，限制见[指纹实现](hardware/led-ir-fingerprint.md#侧指纹) |
| 相机和旧笔解码 | 相机标准替代尚未形成；新内核不调用用户态笔解码 | 不删除仍承担视频、3A、固件初始化或离线对照的实现来换取更小 diff；删除条件以对应系列和[功能对照](hardware/camera-controls.md#现有服务与标准替代)为准 |

运行 `./build.sh check --pattern test_check_patch_series.py` 检查这些规则，Build products 在选择构建目标前运行同一检查。检查按解析后的文件路径合并共享引用，拒绝同一系列重复应用补丁、摘要漂移、矩阵外目标、已登记包的无目标版本、未入系列的补丁和缺少的上游状态。不同文件以 SHA256 和 Git patch-id 检查重复；没有 `diff --git` 头的普通 unified diff 也会参与比较。

旧包的元数据按改动逐步补齐。CI 传入 push 前的提交 `SUNUEFI_PATCH_BASE`，本次新增或修改的补丁与系列必须补齐其包的目标和来源，不能删除已有的 `targets` 逃避检查。未触及的旧包仍校验顺序、路径、摘要和重复变更，输出中显示元数据缺口数。要在本地检查一个提交范围，运行 `SUNUEFI_PATCH_BASE=$(git rev-parse main) ./build.sh check --pattern test_check_patch_series.py`；要要求所有保留补丁完整登记，使用 `SUNUEFI_PATCH_REQUIRE_ALL=1 ./build.sh check --pattern test_check_patch_series.py`。检查不联网，不自动宣称某个修复已上游，也不自动删除补丁。
