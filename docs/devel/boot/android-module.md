# Android 引导助手

SunUEFI 引导助手在 Android 中安装和维护合体 BOOT，提供 Android／UEFI 启动选择及独立存储管理。0.5 模块既支持从当前原厂 BOOT 安装，也支持更新已有 SunUEFI 核心；Android 内核始终取自设备当前 BOOT，安装引导不自动分区。

普通安装步骤见[安装指南](../../user/install-from-artifact.md)。本页说明引导助手的行为和实现。

## 使用

1. 从 **Build products** 的 `SunUEFI-Module` 下载并解压模块包，将里面的 `SunUEFI-Piano-版本.zip` 传到平板。
2. 在正常运行的 Android 中，用 SukiSU 的模块页面安装 ZIP。相同核心只读取、不重写 BOOT；其余情况提示音量＋安装或更新引导，音量－只安装助手。30 秒未选择时只安装助手，可稍后在维护页安装引导。
3. 正常重启，点击模块卡片上的 **打开 WebUI**。

页面分为 **启动、存储、维护** 三页，横屏使用侧边导航，竖屏使用底部导航。启动页提供 Android、UEFI 两条路线。选择 UEFI 后，已安装的 Linux 会默认选中并直接启动；启动菜单作为另一入口保留。没有 Linux 启动文件时，仅显示启动菜单。选择目标后点击 **保存并重启**，以后普通开机和重启沿用该选择，直到再次更改。

新合体 BOOT 在开机时显示 Android／UEFI 选择页，倒计时约三秒后沿用保存的路线。音量上选择 Android，音量下选择 UEFI 并直接启动 Linux，短按并松开电源键确认；这次临时选择不改变模块中保存的路线。画面、音量键的选中反馈、默认 Android 启动，以及临时选择 UEFI 后从 ESP 启动 Linux 均已在设备上使用。原厂 `reboot recovery` 的优先规则保留，固件设置的独立入口尚未开放。

存储页显示独立存储分区的容量，并通过 Android 的本地文件选择器选择 ESP／root 镜像，不要求把文件放进固定目录。0.5.2 起同时接受原始镜像与 Android 稀疏镜像，按展开后的大小检查容量，流式写入并读回校验。分区的名称仍为 `sunuefi_esp` 和 `sunuefi_root`。页面提供镜像安装、创建分区、调整 root 容量和删除并归还 Android 空间的操作入口；执行前展示计划。新设备上的首次分区尚未完成实机验证，不能作为一键首次分区方案。删除并归还空间需要可用的离线扩容路径；原厂跳过加密映射扩容时，模块会在删除前拒绝。机制和容量检查见 [Android 存储扩容](android-storage.md)。

更新镜像时，勾选对应的 **本次更新**，再点击 **安装所选镜像**。只勾选启动镜像（ESP）会保留现有系统和数据；只勾选系统镜像（root）会覆盖系统分区，保留启动文件；同时勾选则写入两者。确认页列出将写入的镜像。重新打开页面时，两项默认都不勾选，之前选择的文件不会自动加入新的更新。

页面支持中文和英文。打开页面只读取轻量状态，通常约 0.2 秒；维护页的 **检查引导** 才进行完整的只读 BOOT 检查，当前设备约两秒。管理器的 **执行** 按钮只显示当前选择和使用提示，不重启，也不输出原生工具的 JSON。

**重新安装引导** 用于 HyperOS 更新后的恢复。若当前 BOOT 仍是匹配的 SunUEFI 合体镜像，操作只重新识别现有引导；若核心版本不同，则保留当前 Android 内核和启动选择后更新；若 OTA 已换回受支持的原厂 BOOT，则从当前活动槽提取新内核，嵌入模块携带的同一套 SunUEFI 核心，并恢复先前选择。它不会自动接管 OTA，也不会预先修改非活动槽。卸载这个助手仅删除模块，现有 BOOT 和启动选择保留；需要返回 Android 时，应先在页面中选择 Android。

SukiSU 安装、模块激活和 WebUI 状态读取已在设备上使用。KernelSU 提供同类 [WebUI 接口](https://kernelsu.org/guide/module-webui.html)；官方 Magisk 的模块 Action 可显示提示，图形页面需要 [MMRL](https://mmrl.dev/) 等宿主，其他宿主尚未做兼容验证。

## BOOT 识别与写入

安装和维护使用同一个通用模块，根据当前 BOOT 选择操作：

| 当前 BOOT | 操作 |
| --- | --- |
| 受支持的原厂 BOOT | `probe` 读取当前 GKI 和头部信息，`repack` 加入开机选择器及当前 UEFI 核心；首次安装默认选择 Android |
| 与模块匹配的合体 BOOT | `adopt` 只读识别并保存管理状态，不重写 BOOT |
| 已知格式的旧合体 BOOT | `upgrade` 完整重建原始 BOOT 并校验包装，再更新核心；保留 Android 内核、原头部和尾部、已有 Root 内容及启动选择 |
| 不受支持或损坏的 BOOT | 停止安装并说明原因，不猜测格式后继续刷写 |

当前支持 Android Boot Header v4 和项目使用的未压缩 ARM64 GKI 布局。Header v3、压缩内核及未知包装不在支持范围内。旧合体 BOOT 使用 `SPLITv1+RSTRv1` 保存原镜像重建信息；更新核心时重新计算启动记录的 generation 和 CRC，保留记录中的目标。

引导写入前核对当前活动槽、ROM、BOOT 内容及源镜像摘要，写入后同步并完整读回。引导操作不写 Recovery、vbmeta、GPT 或 userdata；存储操作由独立工具负责，只在确认具体计划后调整对应文件系统和分区表。旧核心升级已用 96 MiB BOOT 文件验证原镜像重建完全一致；这一结果不代表已经完成所有 HyperOS OTA 场景的实机验证。

只有将原厂 BOOT 安装为合体 BOOT 的 `repack` 路径检查 BL 属性；属性未报告解锁时，按[安装指南中的实体音量键提示](../../user/install-from-artifact.md#1-安装引导助手)确认。`piano-boot-repack confirm-bootloader` 只从平台设备下的 `gpio-keys`／`pmic_resin` 读取三次完整按下和松开，临时独占按键，忽略长按重复和预先按住的键。确认通过后，Shell 只向本次测试安装的子进程传递 `PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER=1`，公共入口会清除外部继承值，不保存永久设置。原生 `repack` 写入前再次检查属性或本次测试确认，型号、活动槽、源摘要、格式、范围、同步及读回检查不变。已有合体 BOOT 的请求更新、核心升级和原样还原仍完整验证当前包装，不再检查 BL 属性；独立存储不在 AVB 校验范围内，也不增加 BL 检查。

源码位于 `android/module/`，引导原生工具位于 `android/native/`，在线 F2FS 调整工具位于 `tools/android/`。`manager.sh` 连接 WebUI 与原生工具：轻量读取使用 `piano-boot-request`；完整检查、识别已有引导、更新、重新打包和改选使用 `piano-boot-repack`。

助手不再向 ESP 写入槽位配置。Linux 切换工具直接读取高通 GPT 的 A/B 属性，校验活动 BOOT，再更新其中的 SunUEFI 请求页；不依赖 Android 与 ESP 同步额外文件。

## 构建

在构建容器中先生成 UEFI 产品，再生成模块：

```sh
./build.sh uefi
./build.sh module
```

`tools/build_android_product_module.py` 从 `artifacts/product/` 提取同一份 `fd.bin`、`app.bin` 和 `shim.bin`，编译前置选择器，并生成以下六个静态 ARM64 工具：

* `piano-boot-repack`：BOOT 识别、包装、更新和还原。
* `piano-boot-request`：读取和修改启动记录。
* `piano-storage`：分区状态、计划及镜像写入。
* `piano-resize-f2fs`：Android 在线 F2FS 缩容。
* `e2fsck`、`resize2fs`：独立 ext4 分区的检查与调整。

默认编译 sysroot 从签名校验后的公共 Debian ARM64 软件包解包，不执行外来架构代码；可通过 `--sysroot` 使用已有 ARM64 开发目录。e2fsprogs 从固定的官方源码构建，ZIP 包含对应许可、来源 URL 和本项目工具源码。无需提供原厂 BOOT、旧 Android 内核或设备私有路径，ZIP 不包含原厂内核。

产物位于 `artifacts/android/`：

* `SunUEFI-Piano-版本.zip`：可安装的模块。
* `manifest.json`：模块、原生工具和同一份 UEFI 产品的构建记录。
* `SHA256SUMS`：ZIP 摘要。

重复构建会复用已下载的工具链目录、重新生成载荷，并在 ZIP 完整生成后替换产物；可通过 `--work`／`--output` 使用其他目录。低层工具保留给已经准备好载荷及原生工具的维护者，检查输入时运行：

```sh
./build.sh module-package --inspect
```

GitHub Actions 的 `uefi` 和 `debian-gnome` 目标同一轮构建模块，独立上传 ZIP、构建清单和摘要。它们使用相同的构建入口，无需先从设备导出已有核心。模块安装合体 BOOT 与首次分区是两件事：安装引导不会创建 ESP／root；存储页的新分区流程仍需实机验证。

不能用离线工具直接调整已挂载的 Android 数据分区，也不能只因原厂库含有扩容代码就假定当前路径可用。存储细节见 [Android 存储扩容](android-storage.md)，BOOT 包装和无损重建见 [原生重打包工具](combined-boot.md)，启动路线与原厂 Recovery 优先规则见 [重启请求](reboot-request.md)。

ESP／root 刷写不再要求模块预先写入蓝牙地址。当前 Linux BSP 在无线初始化时自动读取原厂 persist；原厂地址缺失时可从硬件标识派生稳定地址。公开镜像保持通用，手工刷写也使用同一条初始化路径，见[蓝牙地址](../hardware/bluetooth.md)。
