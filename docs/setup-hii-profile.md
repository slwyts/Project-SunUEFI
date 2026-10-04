# 标准 TianoCore Setup / HII 可选 profile

2026-10-05：标准模块已完整链接。test77 实机最终报告确认 services=Success、load=Success、image=Success、start_called=1、start_returned=0；随后120秒恢复 timer 重启Android，26启动分区校验一致。这证明 UiApp 确实加载并被启动，尚不证明页面布局、按键导航、表单变更或正常退出。变量仍只在 RAM 中。

Setup 期间临时将音量键转换为标准 SCAN_UP/SCAN_DOWN，短按电源释放为 Enter，同时按两音量键为 Escape；返回后恢复 SimpleInit 的既有键码。源码和主机实际输入测试通过，Setup 内实际手动操作仍待验收。

使用入口：`prepare_gui_profile.py --ufs-setup`，隐含标准只读 Filesystems/BlockIO。可另加 `--ufs-shell` 先跑既有自动 Shell 枚举，再启动 Setup；UiApp 的 Continue/退出返回后，RamApp 继续加载原有 RAM SimpleInit。所有启动都发生在 RamApp 的 UFS/DMA/按键 provider 仍存活时。

本次没有运行下面的 prepare/build 命令；统一集成时使用：

```bash
python3 tools/prepare_native_probe.py --group ufs
python3 tools/prepare_gui_profile.py --ufs-setup --return-seconds 120
bash tools/build_stage0.sh gui
```

## 模块与构建依赖

使用固定 Mu_Basecore 的真实模块，不引入 OEM/MsGraphicsPkg 的另一套显示协议，也没有自制页面替代 Setup：

| 模块 | 新增位置 | 职责 |
| --- | --- | --- |
| HiiDatabaseDxe | 已在原 FV | Database/String/Font/ConfigRouting 服务 |
| SetupBrowserDxe | 新增 FV；DSC 已在 Silicium 公共 Components | 产生 FormBrowser2/Ex2，读取 IFR |
| DisplayEngineDxe | 新增 DSC + FV | 产生标准 `gEdkiiFormDisplayEngineProtocolGuid` |
| UiApp | 新增 DSC + FV | 实际 TianoCore Front Page 应用、VFR/字符串包 |
| CustomizedDisplayLib | 新增库映射 | 标准 DisplayEngine 的必须依赖 |
| DeviceManagerUiLib、BootManagerUiLib、BootMaintenanceManagerUiLib | UiApp 的 NULL 库 | 真实设备/启动/维护表单 |
| FileExplorerLib | 新增库映射 | BootMaintenance 的文件选择依赖 |

未加入 BootDiscoveryPolicyUiLib：当前平台没有与该菜单匹配的 BootManagerPolicy 服务和 DynamicHii policy 项，不假装该机制已接通。

UiApp 的 `UpdateFrontPageBannerStrings()` 在缺 SMBIOS 时明确采用默认信息，故初始 Setup 不需要额外 SMBIOS 驱动。本次既没有安装假的 SMBIOS 协议，也没有生成不真实的表项；以后需实际硬件信息时再加入真正的 SmbiosDxe 和平台数据 provider。

`PcdConOutColumn/Row` 显式为 Dynamic，供 UiApp/BootManager 的 `PcdSet32S()` 使用；公共 DSC 原有视频分辨率 PCD 已是 Dynamic。Setup 请求现有唯一真实 GOP 模式 3200×2136，而不是默认的 800×600。80×25 文本模式沿用标准默认，实际布局仍需设备验收。

## 启动与变量状态

`PianoLaunchSetup.c` 先检查真正的 HiiDatabase/String/Font/ConfigRouting、FormBrowser2、FormDisplayEngine、VariableArch/VariableWriteArch 服务。变量 architectural protocols 本身是 NULL-interface marker，不能误判为空服务；其余实际接口为空则拒绝。

变量仍设 `PcdEmuVariableNvModeEnable=TRUE`；loader 在该值不是 true 时拒绝启动。这只是此诊断 profile 的明确约束，不实现硬件变量持久化。UiApp/BootMaintenance 的语言、BootOrder/Boot#### 等变量可以使用标准 API，但模拟 NV store 在 RAM 中，重启后丢失。没有追加 Flash/FVB/FTW 驱动，也没有接管原厂 uefivarstore。

loader 枚举真实 FV2，读取 UiApp GUID `462CAA21-7614-4503-836E-8AB6F4662331` 的 PE32 section，使用该 FV 的 DevicePath + FV file node 调用 `LoadImage`，再检查它确为 UEFI application。没有用 NULL 路径的 RAM 字节冒充可定位的 UiApp 文件。`StartImage` 正常返回/Exit 时由核心卸载应用和 UI 库，RamApp provider 的生命周期保持到其原有 teardown。

loader 在进入/返回时打印设置仅在 RAM 中、重启丢失的提示，并记录：

```text
SUNUEFI_SETUP_PROTOCOL <真实协议名> <状态>
SUNUEFI_SETUP_FV_LOAD ... real_fv_path=1
SUNUEFI_SETUP_START app=UiApp backend=emu-ram persistent=0 hardware_verified=0
SUNUEFI_SETUP_RETURN <状态> backend=emu-ram persistent=0
```

UiApp 会清屏，所以前置文字提示不替代这里的非持久说明或实际保存验收。它的 DisableWatchdog 不取消独立 Stage0 恢复 timer。当前 GPIO 三键可提供方向/Enter，完整文字输入、ESC/F10、子菜单导航仍需要实际验证，不能据主机测试宣称 GUI 可操作。

## 已通过的主机检查

入口：`bash tools/test_uefi_setup.sh`，不会 prepare、完整构建或连接设备。

- 真实 loader 的 ASan/UBSan 行为检查：非 RAM backend 拒绝、缺/空 HII 服务拒绝、marker 协议 NULL interface 接受、无 FV/DevicePath/有效 section 的失败、真实 FV file node、LoadImage 错误、错误 image type 的卸载、StartImage 状态传播与 buffer 释放。
- AARCH64 loader 编译语法检查。
- 5 项固定树元数据检查：Browser/Engine 协议匹配、所有库路径、Dynamic 控制台 PCD、UiApp GUID、SMBIOS 默认分支及不引入 Flash 后端。
- Python 准备脚本语法检查。

这些检查不执行 UiApp 的真实 SendForm，不证明完整链接、显示、输入、变量读回或恢复 timer 成功。下一阶段需先确认实际 FV 收录与 DXE 协议，再记录实机 HII 页面、语言或 BootOrder 的 RAM 变更/读回、退出后 provider 活跃状态，以及恢复后变量未跨重启保存。
