# Android 引导助手

SunUEFI 引导助手在 Android 中提供启动选择和引导维护。目前的 0.3.0 模块用于已经安装 SunUEFI 合体 BOOT 的设备：安装时只读取并识别现有引导，不会首次刷入 UEFI 或修改分区。

## 使用

在 SukiSU 的模块页面安装 ZIP，正常重启后点击模块卡片上的 **打开 WebUI**。页面分为 **启动、存储、维护** 三页，横屏使用侧边导航，竖屏使用底部导航。启动页提供 Android、UEFI 两条路线。选择 UEFI 后，已安装的 Linux 会默认选中并直接启动；启动菜单作为另一入口保留。没有 Linux 启动文件时，仅显示启动菜单。选择目标后点击 **保存并重启**，以后普通开机和重启沿用该选择，直到再次更改。

新合体 BOOT 在开机时显示 Android／UEFI 选择页，倒计时约三秒后沿用保存的路线。音量上选择 Android，音量下选择 UEFI 并直接启动 Linux，短按并松开电源键确认；这次临时选择不改变模块中保存的路线。画面、音量键的选中反馈、默认 Android 启动，以及临时选择 UEFI 后从 ESP 启动 Linux 均已在设备上使用。原厂 `reboot recovery` 的优先规则保留，固件设置的独立入口尚未开放。

存储页显示独立存储分区的容量，并通过 Android 的本地文件选择器选择 ESP／root 镜像，不要求把文件放进固定目录。分区的实际名称仍为 `sunuefi_esp` 和 `sunuefi_root`。页面提供镜像安装、创建分区、调整 root 容量和删除并归还 Android 空间的操作入口；执行前展示计划。新模块中的这些写入流程尚未完成实机验证，不能作为新设备的一键首次分区方案。删除后需正常重启，由原厂系统扩大 Android 文件系统，模块再检查实际容量；具体机制见 [Android 存储扩容](android-storage.md)。

页面支持中文和英文。打开页面只读取轻量状态，通常约 0.2 秒；维护页的 **检查引导** 才进行完整的只读 BOOT 检查，当前设备约两秒。管理器的 **执行** 按钮只显示当前选择和使用提示，不重启，也不输出原生工具的 JSON。

**重新安装引导** 用于 HyperOS 更新后的恢复。若当前 BOOT 仍是匹配的 SunUEFI 合体镜像，操作只重新识别现有引导；若 OTA 已换回受支持的原厂 BOOT，则从当前活动槽提取新内核，嵌入模块携带的同一套 SunUEFI 核心，并恢复先前选择。它不会自动接管 OTA，也不会预先修改非活动槽。卸载这个助手仅删除模块，现有 BOOT 和启动选择保留；需要返回 Android 时，应先在页面中选择 Android。

SukiSU 安装、模块激活和 WebUI 状态读取已在设备上使用。KernelSU 提供同类 [WebUI 接口](https://kernelsu.org/guide/module-webui.html)；官方 Magisk 的模块 Action 可显示提示，图形页面需要 [MMRL](https://mmrl.dev/) 等宿主，其他宿主尚未做兼容验证。

## 维护与构建

源码位于 `android/module/`，引导原生工具位于 `android/native/`，存储原生工具位于 `tools/android/`。`manager.sh` 连接 WebUI 与原生工具：轻量读取使用 `piano-boot-request`；完整检查、识别已有引导、重新打包和改选使用 `piano-boot-repack`。引导写入前仍核对当前槽位、ROM、原镜像与模块拥有的包装，完成后同步并读回；引导操作不写 Recovery、vbmeta、GPT 或 userdata。存储操作由独立工具负责，只在确认具体计划后调整对应文件系统和分区表。内核来自设备当前 BOOT，ZIP 不携带旧 ROM 的原厂内核。

打包有两个独立模式：管理已有合体 BOOT，以及首次安装合体 BOOT。

**已有引导助手** 使用 `adopt` 导出的同一核心载荷目录、对应原生工具及策略文件：

```sh
python3 tools/build_android_module.py \
  --installed-core path/to/exported-payloads \
  --native-tool path/to/piano-boot-repack \
  --native-manifest path/to/policy.json \
  --request-tool path/to/piano-boot-request \
  --storage-tool path/to/piano-storage \
  --resize-tool path/to/piano-resize-f2fs \
  --e2fs-tools path/to/e2fsprogs-build \
  --output artifacts/android/SunUEFI-Piano.zip
```

存储组件包括静态 ARM64 分区／镜像工具、在线 F2FS 调整工具和 e2fsprogs 的 `e2fsck`、`resize2fs`；`--e2fs-tools` 目录还需包含 `NOTICE.e2fsprogs` 与构建 manifest。它们随模块分发，不要求 Android 自带 ext4 调整工具。原厂 F2FS 挂载前扩容仍由系统负责，不能用离线工具直接调整已挂载的 Android 数据分区。

安装程序以只读 `adopt` 识别当前 BOOT，校验嵌入的核心与包内载荷匹配，并保存安装状态。因此它不能用来给没有合体 BOOT 的新设备首次安装。

**首次安装模式** 从产品镜像和 selector 构建完整安装包，仍要求对应工具、载荷及安装检查记录：

```sh
python3 tools/build_android_module.py --inspect

python3 tools/build_android_module.py --product artifacts/product \
  --selector path/to/selector.bin --selector-manifest path/to/selector.json \
  --native-tool path/to/piano-boot-repack --native-manifest path/to/native.json \
  --output artifacts/android/SunUEFI-Piano-install.zip
```

首次安装模式的设备检查尚未完成，因此仍保留安装限制。已有引导助手能正常使用，不表示首次安装模式已准备好；`--candidate` 只输出候选包，不解除写入限制。当前也不应依赖模块完成全新设备的首次分区。

BOOT 包装、无损重建和请求页格式见 [原生重打包工具](android-boot-repack.md)；启动路线与原厂 Recovery 优先规则见 [重启请求](reboot-request.md)。命令分类与两种打包模式记录在 [`native-interface.json`](../../android/module/native-interface.json)。
