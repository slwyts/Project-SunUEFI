# Android 引导助手

SunUEFI 引导助手在 Android 中安装和维护合体 BOOT，提供 Android／UEFI 启动选择及独立存储管理。0.4 模块既支持从当前原厂 BOOT 安装，也支持更新已有 SunUEFI 核心；Android 内核始终取自设备当前 BOOT，安装引导不自动分区。

## 使用

1. 从 **Build products** 的 `piano-android-module-目标-提交号` 下载并解压模块包，将里面的 `SunUEFI-Piano-版本.zip` 传到平板。
2. 在正常运行的 Android 中，用 SukiSU 的模块页面安装 ZIP。安装程序识别当前 BOOT，再安装、更新或保留相同核心。
3. 正常重启，点击模块卡片上的 **打开 WebUI**。

页面分为 **启动、存储、维护** 三页，横屏使用侧边导航，竖屏使用底部导航。启动页提供 Android、UEFI 两条路线。选择 UEFI 后，已安装的 Linux 会默认选中并直接启动；启动菜单作为另一入口保留。没有 Linux 启动文件时，仅显示启动菜单。选择目标后点击 **保存并重启**，以后普通开机和重启沿用该选择，直到再次更改。

新合体 BOOT 在开机时显示 Android／UEFI 选择页，倒计时约三秒后沿用保存的路线。音量上选择 Android，音量下选择 UEFI 并直接启动 Linux，短按并松开电源键确认；这次临时选择不改变模块中保存的路线。画面、音量键的选中反馈、默认 Android 启动，以及临时选择 UEFI 后从 ESP 启动 Linux 均已在设备上使用。原厂 `reboot recovery` 的优先规则保留，固件设置的独立入口尚未开放。

存储页显示独立存储分区的容量，并通过 Android 的本地文件选择器选择 ESP／root 镜像，不要求把文件放进固定目录。分区的实际名称仍为 `sunuefi_esp` 和 `sunuefi_root`。页面提供镜像安装、创建分区、调整 root 容量和删除并归还 Android 空间的操作入口；执行前展示计划。新模块中的这些写入流程尚未完成实机验证，不能作为新设备的一键首次分区方案。删除后需正常重启，由原厂系统扩大 Android 文件系统，模块再检查实际容量；具体机制见 [Android 存储扩容](android-storage.md)。

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

引导写入前核对当前活动槽、ROM、BOOT 实际内容及源镜像摘要，写入后同步并完整读回。引导操作不写 Recovery、vbmeta、GPT 或 userdata；存储操作由独立工具负责，只在确认具体计划后调整对应文件系统和分区表。旧核心升级已用真实 96 MiB BOOT 文件验证原镜像重建完全一致；这一结果不代表已经完成所有 HyperOS OTA 场景的实机验证。

源码位于 `android/module/`，引导原生工具位于 `android/native/`，在线 F2FS 调整工具位于 `tools/android/`。`manager.sh` 连接 WebUI 与原生工具：轻量读取使用 `piano-boot-request`；完整检查、识别已有引导、更新、重新打包和改选使用 `piano-boot-repack`。

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

`build/` 中的工作目录与同名 ZIP 已存在时，构建工具不会覆盖它们；使用新的 `--work`／`--output` 路径即可重新构建。低层工具保留给已经准备好载荷及原生工具的维护者，检查输入时运行：

```sh
./build.sh module-package --inspect
```

GitHub Actions 的 `uefi` 和 `debian-gnome` 目标同一轮构建模块，独立上传 ZIP、构建清单和摘要。它们使用相同的构建入口，无需先从设备导出已有核心。模块安装合体 BOOT 与首次分区是两件事：安装引导不会创建 ESP／root；存储页的新分区流程仍需实机验证。

原厂 F2FS 挂载前扩容仍由系统负责，不能用离线工具直接调整已挂载的 Android 数据分区。存储细节见 [Android 存储扩容](android-storage.md)，BOOT 包装和无损重建见 [原生重打包工具](android-boot-repack.md)，启动路线与原厂 Recovery 优先规则见 [重启请求](reboot-request.md)。
