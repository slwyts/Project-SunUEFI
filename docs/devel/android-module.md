# Android 引导助手

SunUEFI 引导助手在 Android 中提供启动选择和引导维护。目前的 0.2.0 模块用于已经安装 SunUEFI 合体 BOOT 的设备：安装时只读取并识别现有引导，不会首次刷入 UEFI，也不会分区或清除数据。

## 使用

在 SukiSU 的模块页面安装 ZIP，正常重启后点击模块卡片上的 **打开 WebUI**。页面提供 Android、UEFI 两条启动路线。选择 UEFI 后，已安装的 Linux 会默认选中并直接启动；启动菜单作为另一入口保留。没有 Linux 启动文件时，仅显示启动菜单。选择目标后点击 **保存并重启**，以后普通开机和重启沿用该选择，直到再次更改。

原厂 `reboot recovery` 仍进入 Mi Recovery。当前早期选择器没有实现开机按键覆盖：如果保存的是 Android，开机时不能靠音量键临时改走 Linux，需要先在 Android 中更改选择。固件设置的独立入口尚未开放。

页面支持中文和英文。打开页面先读取轻量状态；**检查引导** 才进行完整的只读 BOOT 检查。管理器的 **执行** 按钮只显示当前选择和使用提示，不重启，也不输出原生工具的 JSON。

**重新安装引导** 用于 HyperOS 更新后的恢复。若当前 BOOT 仍是匹配的 SunUEFI 合体镜像，操作只重新识别现有引导；若 OTA 已换回受支持的原厂 BOOT，则从当前活动槽提取新内核，嵌入模块携带的同一套 SunUEFI 核心，并恢复先前选择。它不会自动接管 OTA，也不会预先修改非活动槽。卸载这个助手仅删除模块，现有 BOOT 和启动选择保留；需要返回 Android 时，应先在页面中选择 Android。

SukiSU 安装、模块激活和 WebUI 状态读取已在设备上使用。KernelSU 提供同类 [WebUI 接口](https://kernelsu.org/guide/module-webui.html)；官方 Magisk 的模块 Action 可显示提示，图形页面需要 [MMRL](https://mmrl.dev/) 等宿主，其他宿主尚未做兼容验证。

## 维护与构建

源码位于 `android/module/`，原生工具位于 `android/native/`。`manager.sh` 连接 WebUI 与原生工具：轻量读取使用 `piano-boot-request`；完整检查、识别已有引导、重新打包和改选使用 `piano-boot-repack`。写入前仍核对当前槽位、ROM、原镜像与模块拥有的包装，完成后同步并读回；不写 Recovery、vbmeta、GPT 或 userdata。内核来自设备当前 BOOT，ZIP 不携带旧 ROM 的原厂内核。

打包有两个独立模式，不应混用资格记录。

**已有引导助手** 使用 `adopt` 导出的同一核心载荷目录、对应原生工具及策略文件：

```sh
python3 tools/build_android_module.py \
  --installed-core path/to/exported-payloads \
  --native-tool path/to/piano-boot-repack \
  --native-manifest path/to/policy.json \
  --request-tool path/to/piano-boot-request \
  --output artifacts/android/SunUEFI-Piano.zip
```

安装程序以只读 `adopt` 识别当前 BOOT，校验嵌入的核心与包内载荷匹配，并保存安装状态。因此它不能用来给没有合体 BOOT 的新设备首次安装。

**首次安装模式** 从产品镜像和 selector 构建完整安装包，仍要求对应工具、载荷及设备资格证明：

```sh
python3 tools/build_android_module.py --inspect

python3 tools/build_android_module.py --product artifacts/product \
  --selector path/to/selector.bin --selector-manifest path/to/selector.json \
  --native-tool path/to/piano-boot-repack --native-manifest path/to/native.json \
  --output artifacts/android/SunUEFI-Piano-install.zip
```

该模式的全部资格证明尚未完成，原有安装门禁继续保留。助手的安装成功不等于首次安装模式已经具备这些证明；`--candidate` 也只输出候选包，不解除写入门禁。首次安装同样不提供自动缩容或分区。

BOOT 包装、无损重建和请求页格式见 [原生重打包工具](android-boot-repack.md)；启动路线与原厂 Recovery 优先规则见 [重启请求](reboot-request.md)。命令分类与两种打包模式记录在 [`native-interface.json`](../../android/module/native-interface.json)。
