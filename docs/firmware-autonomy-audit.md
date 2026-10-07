# FAT / UEFI Shell / 固件自主启动审查

审查日期：2026-10-05。以当前工作区和 `sources.lock.json` 固定的 Mu 树为准。此次完成源码和主机检查，未执行 profile prepare、完整固件构建或设备操作。

## 已实现的最小下一块

`tools/prepare_gui_profile.py` 提供三个可组合入口：

| 参数 | 实际行为 |
| --- | --- |
| `--ufs-filesystems` | 隐含 `--ufs-blockio`，在 FV 加入 `EnglishDxe` 和 `FatPkg/EnhancedFatDxe/Fat.inf`；真实 UFS 父句柄递归连接后，检查其 SFS 子句柄 |
| `--ufs-shell` | 隐含 `--ufs-filesystems`；在固定 Mu ShellPkg 源码构建/打包 Shell，先自动执行只读枚举，再继续既有 RAM SimpleInit |
| `--ufs-shell-interactive` | 隐含 `--ufs-shell`；自动枚举后进入交互 Shell，`exit` 后继续 RAM SimpleInit，现有恢复 timer 仍生效 |

准备入口仍要求先选择 `prepare_native_probe.py --group ufs`。本次没有运行下面的准备/构建命令；集成者统一执行：

```bash
python3 tools/prepare_native_probe.py --group ufs
python3 tools/prepare_gui_profile.py --ufs-shell --return-seconds 120
bash tools/build_stage0.sh gui
```

`SiliciumPkg.dsc.inc` 已包含 DiskIoDxe、PartitionDxe、EnglishDxe、Fat 的 Components 和公共库，所以 FAT 阶段主要补 FV 收录。EnglishDxe 不能漏：当前 Fat 的 `FatDriverBindingStart()` 调用 `InitializeUnicodeCollationSupport()`，它只寻找 UnicodeCollation2，缺服务会使 FAT 启动失败。

Shell 的 Components 与库映射原本没有加入平台，新增 `ShellLib`、`ShellCommandLib`、`HandleParsingLib`、`OrderedCollectionLib`，Shell 组件链接 Level1/2/3 和 Driver1 命令库。`PcdShellLibAutoInitialize=FALSE`，SupportLevel=3，ProfileMask=BIT0，关闭分页。没有加入 Debug1、Install1 或网络命令库；底层 UFS 写屏障仍独立存在。

## 真实文件系统和 Shell 证据

新增 `uefi/core/PianoUfsFileSystemProbe.c`：

1. 用 `LocateHandleBuffer(SimpleFileSystem)` 获取真实句柄，再逐节点比较 DevicePath 与 UFS 父路径，只接受其后代。
2. 该句柄必须存在只读 BlockIO；不会探查可写卷。
3. 使用 `OpenVolume`、两次 `GetInfo(FileSystemInfo)` 检查真实文件系统，记录返回状态和 ReadOnly。
4. 用根目录 `Read` 查看最多 64 项；找到首个非空普通文件后，只以 `EFI_FILE_MODE_READ` 打开，读取最多 4 KiB，检查读长并记录 CRC32。没有写入/创建/删除/改名操作。
5. 关闭文件和根目录句柄。介质不存在或没有 SFS 时如实记录 `ufs_sfs=0`，不会安装假文件系统或添加 `fs0`。
6. 暂存最多 32 份紧凑报告，正常 halt/timer 恢复前重新输出，减少大量底层读日志覆盖验收证据的影响；`ufs_sfs` 与 `recorded` 都输出，截断不会隐藏。

标志：`SUNUEFI_UFS_FS_REPORT`、`SUNUEFI_UFS_FS_VOLUME`、`SUNUEFI_UFS_FS_FILE`。空目录或仅有子目录的卷可以证明 OpenVolume/GetInfo/目录读取成功，但没有普通文件内容读取证据；`file=Not Started` 不表示读取成功。

新增 `uefi/core/PianoLaunchShell.c` 从具有真实 FV2 DevicePath 的 FV 读取固定 Shell 文件 GUID `7C04A583-9E3E-4F1C-AD65-E05268D0B4D1`，追加 FV file node 后 `LoadImage/StartImage`。保留真实 LoadedImage 路径，供 Shell 的映射初始化和 image/file path 查询使用。Shell 必须在 RamApp 仍存活时启动，因为 UFS、DMA 和按键服务的代码/数据目前属于 RamApp。

自动启动参数禁用 `startup.nsh`、控制台输入、Ctrl-C 监视、默认映射表和版本输出，然后运行并退出：

```text
map -r
dh -p SimpleFileSystem
drivers
```

每条命令使用新的 Shell 会话，记录独立加载/返回结果，不因缺少 SFS 中止后续 driver 枚举。控制台 OutputString 在命令期间被透明转发，同时复制为 `SUNUEFI_SHELL_OUTPUT` RAM 日志；返回后恢复原回调。没有在 UFS 写诊断脚本，也不依赖预先存在的 `fs0:`。交互模式仍依赖真正的文字输入设备；当前 GPIO 三键可导航 SimpleInit，但不能提供完整字母输入。

## 当前已验证能力的界限

第 57 次日志 `private/analysis/ramlog-test-57/uefi.txt` 证明 6 父 + 133 GPT 分区 = 139 BlockIO。已有标准 DiskIo/Partition 连接证据不证明 FAT、Shell 或 UFS 文件加载成功。

本次只读解析第 57 次 Android 对照 GPT 条目：133 分区没有标准 ESP 类型 `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`。这不排除其他分区内部存在 FAT；Fat 可以识别真实 FAT 内容。若新 profile 只显示 `blk`、没有 `fs`，这是有效结果。不能把任一原有 Android 分区直接格式化为 ESP，也不能因为名为 `uefivarstore` 就认定它可被本固件接管。

SimpleInit 的 UEFI volume 层已经枚举 PartitionInfo/BlockIO/DiskIO/SFS；真实 FAT 接通后，它可直接看到卷。此次仍使用经过既有 hash 校验的 RAM SimpleInit，尚未实现 `\EFI\BOOT\BOOTAA64.EFI` 或指定路径的 UFS 自主加载。

## 生命周期修正与 EBS 未完成工作

`uefi/core/PianoUfsReadOnlyDma.c:PianoUfsBlockIoStop()` 原先先 `HaltService()`，把父介质置为不存在，随后才 DisconnectController。现改成：

```text
DisconnectController 全部父设备，消费者仍可访问只读介质
→ HaltService / MediaPresent=FALSE
→ 关闭 EBS event
→ 卸载 BlockIO/DevicePath 和 shutdown protocol
→ 恢复/停止队列、解除 SMMU 映射、释放 DMA、关闭时钟
```

DisconnectController 失败时立即走保留资源的 cold-reset 路径，不能继续卸载/free 仍可能被消费者引用的数据。timer 与 EBS 仍直接使用 halt-only，不在其回调里 DisconnectController、卸载协议或释放内存。

当前 EBS hook 已有且此次保留；第 57 次镜像未包含该后续 hook，不能声称已实机验证 OS 交接。FAT 的 flush 注册在 BeforeExitBootServices，UFS halt 注册在 ExitBootServices，保持正确的阶段关系。

未来 OS 交接仍需证明队列运行位/doorbell/task-management 均停止，且 DMA/SMMU 页表不会在硬件仍引用时被 OS 回收。当前 DMA 分配为 `EfiBootServicesData`，halt-only 不卸载 SMMU，不等于为 OS 保留了这些页。当前 Quiesce 的循环只检查 slot0 doorbell，尚未验证所有停止读回。EBS 中仍有 `gBS->Stall` 路径；它本身不是 timer-event 等待，但外部服务调用应收敛为本地有界轮询/TimerLib，不应依赖其他驱动回调的先后。

## 变量后端与自主启动顺序

平台和公共 DSC 当前均设 `PcdEmuVariableNvModeEnable=TRUE`。`VariableNonVolatile.c:304` 初始化模拟 NV store；`VariableDxe.c:635` 明确此模式不依赖 FVB/FTW。NV 属性表示 API 语义，冷重启后 RAM store 丢失，不能据此宣称 BootOrder/Boot#### 已持久化。

FAT、Shell 和初始 HII 都可以在该模式下工作。保留它，避免同时开启未经审查的 Flash/FVB/FTW 或原厂 `uefivarstore` 后端。后续变量持久化必须单独确定可写存储所有权、格式、掉电恢复、写范围和 runtime 生命周期。尤其不能用现有 Boot Services FAT/BlockIO 接口直接承担 EBS 后 Runtime `SetVariable()` 的磁盘落盘。

建议后续顺序：真实 FAT/SFS → Shell 及文件只读验证 → 从真实文件 LoadImage 的受控路径 → UFS provider 独立 DXE DriverBinding 与安全 OS 交接 → 专用持久变量设计 → 完整自主启动。

## HII / Setup 的具体下一步

当前 FV 已包含 HiiDatabaseDxe，因此 HII Database/String/ConfigRouting 基础不是空缺。SetupBrowser 只在公共 DSC Components，尚未入 gui FV；浏览器存在也不会自动生成设置界面。

若要先做标准文字 Setup，可在独立 flag 下增加：

```text
FDF: MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
FDF + DSC: MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
FDF + DSC: MdeModulePkg/Application/UiApp/UiApp.inf
LibraryClasses: CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
```

当前树 SetupBrowser 消费 `gEdkiiFormDisplayEngineProtocolGuid`，标准 DisplayEngine 匹配。不要误选 Mu `MsGraphicsPkg/DisplayEngineDxe` 与 OEM FrontPage 的另一套 UI。UiApp 在 SMBIOS 缺失时使用默认 banner，因此 SMBIOS 不是最初显示 Setup 的强制条件。完整菜单可再照同树 `MdeModulePkg.dsc:405` 加入 DeviceManagerUiLib、BootManagerUiLib、BootMaintenanceManagerUiLib，并映射 FileExplorerLib。初始项目设置可以用独立 VFR/UNI + HiiConfigAccess，仅保存到 RAM；HII 和变量后端是否持久化是两个独立验收点。

## 主机验证与实机验收

本次通过：

可复现入口：`bash tests/native/test_ufs_firmware.sh`，只运行主机行为/ABI检查，不 prepare、不完整构建、不连接设备。

- Python 准备脚本语法及 `--help`。
- 两份新诊断 C，以及 `PIANO_UFS_BLOCKIO + PIANO_UFS_FILESYSTEMS` 下真实 UFS 源码的 AARCH64 编译语法检查。
- `tests/native/test_ufs_filesystems.c` 的 ASan/UBSan：真实路径继承、无 SFS、可写卷拒绝、OpenVolume/GetInfo/目录/文件读取 CRC、异常元数据和关闭/free。
- `tests/native/test_ufs_blockio_lifetime.c` 的 ASan/UBSan：先断消费者、失败保留资源走恢复、EBS halt-only。

全固件构建和实机启动仍由集成者执行。验收需包含新增模块确实在 FV、Shell 加载/返回和自动命令输出、真实 `ufs_sfs` 数量、每个卷 OpenVolume/GetInfo/读取结果，以及最终恢复后 GPT 和启动分区哈希保持。没有真实 FAT 卷时应把“标准链路已接通，但 UFS 文件载入未完成”写清楚。

## 官方资料

- [UEFI 2.11 Boot Manager：BlockIO 路径的递归 ConnectController 与 SFS 文件启动](https://uefi.org/specs/UEFI/2.11/03_Boot_Manager.html)
- [UEFI 2.11 Boot Services：ExitBootServices 事件、内存和 timer 限制](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html)
- [TianoCore Shell 的启动、命令行和映射路径](https://github.com/tianocore/edk2/blob/master/ShellPkg/Application/Shell/Shell.c)
- [TianoCore Shell 组件与命令库配置](https://github.com/tianocore/edk2/blob/master/ShellPkg/ShellPkg.dsc)

模块名称和库依赖以本地固定 Mu 源码为准，官方当前 master 仅用于对照规范/行为，不能替代当前构建树的 INF/DSC。
