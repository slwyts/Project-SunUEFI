# Project SunUEFI — Xiaomi Pad 8 Pro (`piano`)

目标是在保留现有 Android 系统和数据的前提下，研究并移植 ARM64 UEFI，随后从 RAM 或外接介质启动 Linux / Windows PE。

**最新实机结果（第114次）：同一产品从真实ESP启动Kernel69、真实ext4根系统和GNOME。** 修复合法boot_a/boot_b优先级变化误触发旧GPT保护后，产品Fastboot枚举和256KiB日志上传实测成功；显示AHB enable已保留，但UEFI物理画面仍白。Linux约2分10秒进入图形目标，显示/触控/键盘/无线/ADSP/音频服务均自动启动。用户确认键盘、触控板、Wi-Fi联网及蓝牙发现设备可用；触屏长按能弹出菜单。普通GNOME左右扬声器测试在补装libcanberra-pulse后正常；DMIC1的6秒RAM录音回放可辨认音乐，但底噪大，录音质量和输入音量条仍未闭环。当前保持Linux会话，recovery仍是已恢复且校验一致的原厂版本。

**触控速率实测（第111次会话）：** 原厂固件THP帧中的扫描率字段为144 Hz，原始帧交付143.998 Hz，稳定期uinput报告约144 Hz。43.89秒单消费者观测收到6316帧，序号缺失、校验错误及输入写入错误均为0；主机算法平均12.73微秒，SPI完成到uinput写入平均1.54毫秒、最大7.05毫秒。这不包含传感器扫描等待和GNOME呈现延迟。可复用测量已加入运行时helper（默认关闭）；原厂游戏模式getter及可回退setter正在适配，尚未启用或实测360 Hz。

**无线与音频（第111次会话）：** PCIe已枚举PEACH Wi-Fi，真实网卡`wlp1s0`已能扫描附近热点；BT的PERI/控制器固件已完成加载，`hci0` powered，扫描命令成功。ADSP、AudioReach、四个FS19xx放大器及真实ALSA声卡已注册，PipeWire已识别扬声器和内置麦克风，播放/录音PCM DMA指针持续推进。无线、ADSP、音频服务已在Linux root中启用；网络关联/蓝牙配对、真实听音/麦克风信号及冷启动恢复尚需验收。Python sysfs读取丢失EAGAIN的问题已修复，仍严格验证实际identity/stage1路由，没有跳过DMA检查。

**下一次启动候选：** Stable `7.2.6-piano-gnome-00069-gefe5734c2451`、匹配1637个模块、小型磁盘bootstrap及键盘supplier修正均已安装到真实root/ESP并读回确认；入口仍为`\EFI\Piano\stable\boot.img`。新增THP控制器getter、0/1游戏模式和已确认旧状态的回退接口；尚未实机启动该内核、切换模式或证明360 Hz。Recovery尝试发现原厂vbmeta对recovery的AVB chain依赖：无footer产品使加载失败并标记A槽unbootable。已恢复原厂recovery、清除A槽失败标记，完整读回匹配且Android boot_completed=1；后续结构有效的unsigned AVB容器也被ABL拒绝；已再次恢复原厂recovery、清除残留boot-recovery命令，普通磁盘Android重启已验证。缺footer不是全部问题，实际recovery加载/认证差异仍未定位，尚不能宣称无电脑入口可用。唯一product已加入明确recovery入口3秒后自动Stable的策略；正常/未知入口保留SimpleInit，侧键/F12/USB请求可覆盖默认，实机recovery策略仍待验收。

**实用兼容修正（待实机验收）：** 为先恢复已验证的ABL继承显示路径，唯一产品使用严格固定原始/派生身份的ClockDxe兼容派生，保留CESTA初始化获取的4个显示和3个相机时钟引用，ABI/callback table不变，不宣称自主显示驱动或owned显示退休完成。另已读回确认，槽位修复只改变LUN4 `boot_a`/`boot_b` 的4位priority；旧optional product-volume校验因此在USB/菜单前停止。新校验只允许这两条固定记录的priority位变化，其他字段、CRC及写入保护不变；真实GPT夹具现返回未配置卷的NOT_FOUND而不是隔离。两项都仍需同一镜像的菜单、Fastboot及ESP Linux联合实测。

按用户授权已在线缩小 userdata，并新增 `sunuefi_esp`（512 MiB FAT32）和 `sunuefi_linux`（63.5 GiB ext4），共64 GiB。Debian 13/GNOME 已写入 Linux 分区。两次 Android 恢复已验证，`/data` 约397 GiB总容量、60 GiB已用。只读回读核对了分区边界、文件系统标识、systemd/GNOME Shell SHA256及根分区配置。该次Linux分区部署未刷写 Android boot/recovery/system。证据保存在 `private/provisioning/sunuefi-linux64-plan-20261006/`、`private/analysis/linux-disk-test107/` 和 `private/analysis/ramlog-test-107/`。

真实ESP已安装Stable的 `\EFI\Piano\stable\boot.img`、Image、DTB和小型initramfs，13个文件全部读回校验，证据 `artifacts/linux-assembled/piano-disk-esp-20261007-r5/install-result.json`。Stable启动镜像SHA为 `bb0b05532fe55279e0e40a19d45f76c407513a7ee6cde00e39f23715903cb938`；同一内核及配套模块已在root分区。单一产品固件的本地ESP加载入口已由第111次启动验证，菜单复用真实文件source和原OS交接。Next文件已放ESP，但其强制RAM根和磁盘bootstrap仍需改造，尚不可选为可运行的磁盘系统。

**唯一产品候选：`artifacts/product/PianoUEFI-product.img`，状态为 `INCOMPLETE_NOT_RELEASE`。** 同一份核心集成 TianoCore 启动画面、默认 SimpleInit、F12 Setup、标准 Shell 和驻留 Fastboot；没有诊断自动重启计时器。第105次构建通过405项主机测试、固件编译和输入一致性验证，实机验证256KiB日志及统一退出重启；第103次另已验证3200×2136完整BMP上传。第111次当前镜像SHA256为 `4eacaceee862c107e1b98e578efe881153beda6bd2e430fd739a831cd54f0309`，封存在 `artifacts/tests/stage0-test-111/`。构建、必需功能与真实后端状态见 [唯一产品构建](docs/piano-product-build.md)。

已有 UFS 证据包括6个LUN和GPT读取、139个只读BlockIO句柄、7个只读SFS卷及标准Shell枚举；第80/86次专用区域写入实验完成备份、写入、读回和恢复，第91次标准fetch与USB/UFS联合关闭通过。UEFI 原分区访问仍只读；新增 Linux 分区已按上文授权修改 GPT，独立的产品FAT/NV容器仍未配置，EFI变量仍在RAM中。触摸真实输入、官方键盘/触控板、USB Host、通用OS启动和1GiB下载仍有未完成后端。早期诊断镜像作为实验封存，不构成多套最终产品功能集。

SimpleInit 已包含“固件设置（BIOS）”“进入 UEFI Shell”，标准 `fastboot oem setup/shell/simpleinit` 导航走同一父核心和后台服务。当前冷映射保护 ADSP/HWFence 与低堆边界，保留上方47MiB资源，并按原厂描述加入独立显示 MMIO 资源；这些变化没有授权高DDR分配。完整 Stable/Next Linux 和RAM发行版已有主机构建候选，真实大块Conventional内存发布、加载来源与OS交接仍未闭环，GNOME和Windows启动尚未实机验证。见 [内存契约](docs/piano-platform-memory-contract.md) 与 [完整DDR生产者缺口](docs/piano-full-ddr-producer-gaps.md)。

产品菜单原“继续启动”已明确改为“返回 Android（重启）”，经真实Continue请求、GUI清理和全部设备退休后冷重启；尚不表示默认OSloader已就绪。真实USB已应答动作优先于较早UI请求。官方键盘/触控板的协议生产者生命周期已完成源码测试，真实SE6传输仍未就绪，不发布虚拟输入。

已准备 [专用存储提案](docs/piano-product-storage-proposal.md) 和 [标准NV后端](docs/piano-persistent-nv-backend.md)：固定14MiB容器提供有边界的FAT与双槽日志，PC提案与实际C provider互操作通过。该14MiB方案未写介质；它与已安装的64GiB Linux分区独立，标准变量驱动早期初始化仍未完成，不能把后端源码和断电模拟当作实机持久化。

详见 [BlockIO / USB进展](docs/blockio-usb-progress.md)、[受控UFS写入证据](docs/ufs-controlled-write-transport.md) 和 [Linux EFI交接审查](docs/linux-efi-handoff-audit.md)。stable/next独立内核已分别通过第71/73次原生ARM64 RAM启动；第85次标准EFI路径启动8CPU并完成MM/console，抓到固定TrustUI CMA缺失vmemmap导致panic；第88次补入两个占用CMA区域后越过原故障，但因普通可分配内存不足在内核初始化时OOM。EFI路径RAM用户态PID1尚未成功。

全部功能仍是目标，按下表分别记录实测状态。驱动加载成功不等于硬件传输成功。

| 功能 | 当前证据 | 未完成部分 |
| --- | --- | --- |
| GOP / 中文显示 | 3200×2136 继承显存模式；中文 GUI、字号 72/64、GOP Blt 和 RAM 截图通过 | 更多模式和显示硬件重新初始化；近期部分用户观察为灰屏，RAM 截图不能证明面板正在扫描输出 |
| 实体按键 | 第 23–24 次实测音量移动焦点，两次短按电源选中并执行，进入工具主界面 | 更多快捷键及长按策略 |
| 官方键盘 / 触控板 | 软件报告与输入层、固定GENI PIO模块及受保护只读SE6快照固件已构建，均未实机验收 | 真实clock/FW/PIO报告、完整键盘与触控板输入 |
| USB 设备 / PC 调试 | 第82–84次SuperSpeed、RAM/日志、reboot及全帧BMP实测；第91次只读fetch与联合关闭通过 | 唯一产品已集成驻留服务，跨SimpleInit/Setup/Shell实机验收、目标重启、受控flash及自主PHY/Type-C管理待做 |
| USB 主机 | 标准PCI_IO facade的地址、映射与生命周期主机测试通过 | NC common-buffer后端、主机角色/VBUS、XHCI及外设实测 |
| 串口调试 | ramoops RAM 日志及重启后 ADB 回收已实测 | 物理 UART 和实时 USB 日志；RAM SerialPortLib 不是物理串口 |
| 触屏 | NT36532 cascade 的固件、地址表、SPI 引擎与引脚已核对；RAM 固件读取通路实测 | GPI/PAS 与 DMA、真实触点读取、AbsolutePointer 发布；SimpleInit 同坐标抬起和旋转缩放已修复并编译 |
| DMA / SMMU | PA↔IOVA、缓存同步、自有SID60/40上下文；UFS读写与USB EP0真实传输/解除读回 | GPI真实传输、NC common-buffer、64位高IOVA实测 |
| UFS | 139个只读BlockIO、7个SFS卷、Shell枚举；第80次固定块与第86次限定窗口FAT8193-byte文件写入、读回及完整恢复实测 | Shell文件操作、永久测试卷、自主启动及OS交接 |
| SimpleInit / Setup / Shell | 中文放大菜单和工具主界面已由用户确认；产品集成默认SimpleInit、F12 Setup、标准Shell及共同返回策略 | 产品联合实机验收、真实触摸/键盘和文件维护功能 |
| Linux | 原机GKI和独立stable/next RAM initramfs BusyBox PID1；第85/88次EFI内核启动且早期故障已定位 | EFI普通内存不足、用户态启动、完整DRAM map、主线整机驱动及真实发行版 |
| Windows PE | 未启动 | ARM64 平台 ACPI、存储、USB、显示和启动链验证 |


**此前已验证：已连续多次实机验证自编译 piano UEFI → 原机 GKI → 独立最小 RAM initramfs / BusyBox PID 1 → 180 秒后恢复 Android。第 16 次记录了正确的 HWID、71 个模块正常加载，以及仅 RAM/虚拟文件系统的挂载表。USB 控制器仍延迟探测，没有可用 USB 串口或 Linux 显示驱动；完整发行版和 Windows PE 尚未运行。当前成功路径是 UEFI 内嵌 ARM64 加载器的原生交接；通用 EFI-stub 路径仍待调试。第 16 次恢复后 26 个启动相关分区哈希全部一致，root 与 A 槽位正常。**

## 实机已验证

| 项目 | 结果 |
| --- | --- |
| 产品 / 代号 | 小米平板 8 Pro，`25091RP04C` / `piano` |
| SoC / 平台 | `SM8750P` / `sun`，`soc_id=639` |
| CPU / 内存 | 8 核；Android 可见内存约 15 GiB，16 GB 机型 |
| 系统 | Android 16，`OS3.0.309.0.WPYCNXM` |
| 当前内核 | `6.6.118-android15-8`，ARM64，KernelSU root 可用 |
| Bootloader | 已解锁，原机 fastboot `unlocked: yes` |
| 安全启动 | 原机 fastboot `secure: yes`；解锁不能用于推断底层固件可随意替换 |
| 当前槽位 / boot 大小 | A / 96 MiB |
| fastboot 下载上限 | 768 MiB |
| 临时内存启动 | 已用**当前 boot_a 的原样备份**实测 `fastboot boot`，`Sending` / `Booting` 都返回 `OKAY` |
| 返回 Android | 实测 `sys.boot_completed=1`，槽位仍为 A，KernelSU root 可用 |
| 自编译 UEFI | 第 7 次最小版原视频确认 BDS 控制台；第 8 次 ramoops 确认 `PIANO_STAGE0_CONSOLE_READY EL1` |
| 自动恢复 | 多次临时启动后自动恢复 Android；已核对 ADB 启动完成、root 和 A 槽位。第 7 次 BDS 无 OS 路径约 10 秒后关机，不能与 45 秒计时器混同 |
| Linux RAM 启动 | 第 13–16 次重复成功；第 16 次明确记录 PID 1、正确 HWID、71 个模块及 RAM 挂载列表 |
| 实验后分区校验 | 最近一次第91次，26启动分区SHA一致，root与A槽正常；完整测试gap/GPT/邻块在FAT试验后恢复一致 |

截至第91次的历史范围：启动分区只读，当时没有执行 `flash`、`erase`、重分区、改槽或Bootloader锁定/解锁。后来经授权的64GiB Linux分区创建与写入见顶部最新状态。第80次仅在独立备份且live gate通过的测试gap内进行固定块写入，并完成恢复及Android独立校验。Linux 对照实验使用原样启动内核和 RAM initramfs。没有主动写入用户数据、加密元数据、持久化校准或密钥分区，也没有复制其内容；正常 Android 启动仍会自行更新运行数据。分区哈希校验范围为上述 26 个启动相关分区。

原始证据保存在 `private/`，默认不纳入版本控制。26 个 A/B 启动相关分区逐一通过设备端与电脑端 SHA-256 比对；完整采集为 36 个文件、743,577,789 字节。该备份用于分析和恢复启动固件，不是用户数据备份。

## 已实现的诊断目标

`platforms/pianoPkg/` 基于 [Mu-Silicium 的 SM8750 Pakala 平台和 OnePlus 13 移植](https://github.com/Project-Silicium/Mu-Silicium/tree/66e7bd1e7bcb757d4b28629bd6409d7209d3b242/Platforms/OnePlus/dodgePkg)。平台配置取自本机 `xbl_config_a` 中的 Qualcomm UEFI DT 配置，没有直接采用手机内存布局。

主要证据与实现：

- `DXE_Heap`：本机为 `0xBD930000`、大小 `0x1A6D0000`；一加参考实现为 `0xB90C0000`，不能直接照搬。
- 原机 UEFI FD 窗口为 `0xA7000000`、4 MiB。实验目标沿用同芯片 BootShim 的布局，将 `0xA7100000` 起的 3 MiB 用作自编译 FD；该布局已在最小诊断版实机执行。
- 栈起点 `0xA760D000`；GICD `0x16000000`，GICR `0x16080000`；显示保留区 `0xFC800000`、43 MiB。
- 生成 41 项内存描述符和原机配置表，原生 10 个早期 DXE 模块直接从本机 UEFI 提取，保留原始 DEPEX，未使用其他设备的补丁二进制。
- SimpleFB 候选模式为 `3200×2136`、32 位像素。Android 物理屏幕报告 `2136×3200`，设备树的双 DSI 单链路为 `1600×2136`；文字渲染已由原视频确认；显示协议参数与方向仍需进一步核对。
- 诊断代码设计为到达 BDS 控制台后打印平台、当前异常级、GOP 和 framebuffer 地址，并请求 45 秒后的冷重启。第 6 次 SCM/TZ 断言发生在此回调前；第 7 次最小版已进入回调，第 8 次读回 EL1 与运行时 DTB/initrd 参数。
- 最早stage0固件FV不含UFS/磁盘/分区；后续profile按明确选项加入本地UFS及标准DiskIo/Partition/FAT/Shell/Setup。所有profile仍不含原版CapsuleRuntimeDxe或UFP更新组件；必需的 Capsule Architectural Protocol 由本地空驱动提供，两个 Capsule 服务均直接返回 `EFI_UNSUPPORTED`。变量启用 `PcdEmuVariableNvModeEnable=TRUE`。这降低了测试影响范围，**不能据此保证未验证原生模块没有副作用**。
- 两个 FV 必须设置 `READ_ENABLED_CAP` 和 `READ_STATUS`。前两次候选遗漏 `READ_STATUS`，DXE 的 `FvReadFile()` 因此返回 `EFI_ACCESS_DENIED`；封装脚本已增加检查。RTC、Monotonic Counter、Capsule 空接口及完整的早期驱动 APRIORI 顺序也已补齐。
- 启动用 DTB 来自本机完整设备树，并清除了旧 initrd 指针、随机种子和 bootargs。DTBO overlay 只作板级信息验证，不能充当完整 base DTB。下游 Android DTB 不能直接作为主线 Linux 的板级支持。

生成物在 `artifacts/stage0/`：

- `piano-stage0.fd`：3 MiB UEFI 固件。
- `BootShim.bin`：ARM64 跳转/搬运代码。
- `piano-stage0-UNTESTED.img`：Android boot header v4 的临时启动候选镜像。
- `manifest.json`：镜像大小、哈希、封装检查与未验证状态。

stage0基础profile用于初始化/显示诊断；linux profile按manifest封装明确内核/DTB/initramfs。尚无Windows启动管理器实测。文件名中的 `UNTESTED` 是封装脚本的保守候选标签；每次实际发送的版本、哈希和结果分别保存在 `artifacts/tests/` 和 `private/analysis/stage0-test-*.json`，不能仅根据文件名或 fastboot 的 `OKAY` 判断移植成功。

## 开发与复现

所有构建在电脑上进行。脚本不执行设备刷写。

```bash
# 首次只读采集；输出目录必须不存在。
python3 tools/capture_device.py --output private/captures/新的采集目录

# 离线分析。
python3 tools/analyze_capture.py private/captures/2026-10-03-piano --output private/analysis

# 原生 UEFI 提取工具安装在项目 venv 中。
.venv/bin/uefi-firmware-parser -b -e -q \
  -o private/uefi-extracted \
  private/captures/2026-10-03-piano/uefi_a.img

# 准备平台：校验采集哈希后生成配置、原生模块 INF 和诊断 FDF。
python3 tools/prepare_piano.py \
  --capture private/captures/2026-10-03-piano \
  --extracted private/uefi-extracted

# 编译与封装。两者都只操作电脑文件。
bash tools/build_stage0.sh
python3 tools/package_stage0.py

# 默认仅检查候选，不触碰设备。
python3 tools/ram_boot_stage0.py --test-id 100

# 仅在有人守在平板旁、且已准备好按键恢复时显式执行。
# 该操作会重启平板并执行实验固件；不是刷写。
python3 tools/ram_boot_stage0.py --test-id 100 --execute
```

参考源码放在 `upstream/Mu-Silicium`，主仓库固定提交 `66e7bd1e7bcb757d4b28629bd6409d7209d3b242`。Python 工具在 `.venv/`；LLVM、LLD、NASM、ARM64 binutils 和 iASL 放在 `build/host-tools/`，没有运行参考脚本中的系统全量升级。BaseTools 从源码编译，避开本机缺少 Mono 的 NuGet 下载流程。

第三方固件二进制和采集材料仅留在本地。参考仓库的许可与版权说明保留在各自目录中。

## 后续路线和边界

1. **初始化/显示诊断已到达 BDS**：最小诊断目标只保留原生 EnvDxeEnhanced，排除第 6 次录像中失败的 SCM/TZ 及其他非必需原生驱动。RAM 日志已可自动取回，常规测试无需录像；早期硬卡住仍可能需要按键恢复。
2. **Linux**：原机 GKI + 最小 RAM initramfs 已启动，当前继续补齐 USB 交互，默认不访问内置 UFS。需要补齐 piano 的主线板级设备树或验证原机 GKI + 匹配 vendor 模块的路线，再接 USB 控制台/输入和外接介质。通用 ARM64 ISO 不代表具备 SM8750 平板驱动。
3. **ARM64 WinPE**：需要板级 ACPI（至少 MADT/GTDT/FADT/DSDT，必要时 IORT）、GOP、内存/中断和 PSCI 兼容性，再接启动介质与驱动。仅有 UEFI 菜单不能证明 Windows 内核能启动。

检索到的同芯片 [OnePlus 13 状态表](https://github.com/Project-Silicium/Mu-Silicium/blob/66e7bd1e7bcb757d4b28629bd6409d7209d3b242/Status.md) 记录 Linux 已启动、Windows 未启动，并且 USB Host 尚不可用。这是参考设备的项目报告，不是 piano 的实测结果。

Windows 介质必须使用 ARM64 版本。[Microsoft 的 WinPE 文档](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-adding-powershell-support-to-windows-pe?view=windows-11) 给出 ARM64 介质工具流程；[WinPE 本身不支持跨架构运行 AMD64 应用](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro?view=windows-10)。

其他可复核来源：

- [Mu-Silicium 的 Pakala SoC 源码](https://github.com/Project-Silicium/Mu-Silicium/tree/66e7bd1e7bcb757d4b28629bd6409d7209d3b242/Silicon/Qualcomm/PakalaPkg)。
- [小米公开内核 `piano-w-oss`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/piano-w-oss)，已固定查询提交 `45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71`，该分支有 `build.config.msm.piano` 和 `piano.bzl`。
- [Linux 上游 SM8750 DTSI](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8750.dtsi) 及 [SM8750 主线开发仓库](https://github.com/sm8750-mainline/linux)。本轮查询上游 qcom DTS 目录只有 SM8750 MTP/QRD 板，未发现 piano 板级文件。
- [小米 15 SM8750 的公开 manifest](https://github.com/wumai2580/xiaomi-dada-manifest) 提供了一条实验路线，但其引用的设备移植仓库本轮无法匿名读取，不能当作可复现的成品。

不通过替换 `uefi` / `abl` / `xbl` / `hyp`、擦除固件、解锁/锁定 Bootloader 或重分区来推进研究。

## Linux 阶段的当前准备

已把原机 `boot_a` 内核检查为 ARM64 PE/COFF EFI-stub（Machine `0xAA64`），在电脑上准备了约 39 MB 的 RAM 载荷：原样内核加约 6.75 MB 的最小 initramfs。initramfs 包含 ARM64 静态 BusyBox，以及从同机固件解析出的 47 个 USB/PHY/时钟等模块和依赖；压缩与 CPIO 格式已检查。它只设计 USB ACM 串口，不提供 USB 大容量存储，也不挂载持久化文件系统，带 180 秒诊断重启脚本。**第 13 次原生交接已运行该最小 initramfs；USB 串口尚未工作。第 14 次候选增加到 65 个模块，包含设备树要求的硬件自旋锁、PDC、M31 PHY、I²C、总线和 IOMMU 等依赖，未加入内置存储驱动。**

`bootprofiles/linux-ram/init` 和 `tools/make_linux_initramfs.py` 保存了构建逻辑；输出在 `artifacts/linux-ram/`。BusyBox 来自 Alpine 官方 ARM64 仓库，版本与下载来源记录在 `build/linux-ram/busybox-source.json`，没有将发行版的通用内核替换到设备。

Linux 启动需要运行时 DTB 和 initrd 交接参数。独立 `pianoProbePkg` / `bootprofiles/handoff/` 保留了 BootShim 进入时的 x0 和 EL。封装脚本强制检查 `_Payload` 偏移等于 BootShim 完整长度，避免第 4/5 次汇编常量池引起的 16 字节错位。最小版只保留原生 EnvDxeEnhanced，以避免 ABL 已交接内核环境后重新初始化 SCM/TZ 导致的断言。

自动诊断通过本机 ramoops 的 console RAM 环形缓冲区保存 UEFI DEBUG 输出（`0xA3500000`，前 2 MiB）；检查原有签名、边界和写入位置，不清空原日志，不触碰后 2 MiB pmsg。回到 Android 后只读 `/sys/fs/pstore/console-ramoops-0`。第 8 次已实测读到 EL1、有效 DTB `0xB5B76000` 及原始 initrd 参数。日志位于 `private/analysis/ramlog-test-*/`。

第 9 次使用 Android boot header v4 时，ABL 仍采用现有 init_boot，未传入镜像内的实验 ramdisk，故显示 `Linux payload was not found`。第 10 次改为 header v3 后，运行时 initrd 长度变为 72,453,165 字节，内核 36,866,560 字节和压缩 initramfs 2,314,601 字节均通过 SHA-256。第 11 次进一步确认 Linux EFI-stub 调用了 LoadFile2 并收到完整 initramfs，随后出现 ExitBootServices 回调；没有 USB ACM 或新内核日志，不能据此宣称 Linux 启动成功。

`pianoLinuxPkg` 实现 FV 内嵌 LinuxRamBoot 应用。默认走 EFI-stub；`--raw` 为独立 ARM64 对照：RAM 中放置原机 Image、修改后的运行时 DTB 与最小 initramfs，退出 Boot Services、处理缓存/MMU，然后按 x0=DTB、x1..x3=0 的约定跳入内核。第 12 次日志确认复制载荷和 ExitBootServices 已完成，但 `WriteBackInvalidateDataCache()` 在当前库中是 ASSERT 占位实现，停在真正进入内核之前。已修正为范围刷新，并在无栈汇编关闭 MMU 后写入独立 RAM 检查点；第 13 次修复版成功运行 Linux：日志中 `Kernel command line` 包含实验 `rdinit=/init`，`Run /init as init process` 后出现用户脚本触发的 insmod，180.44 秒由脚本重启。新 Linux 初始化 ramoops 后覆盖了 UEFI 阶段日志，这是正常的日志换代；完整证据位于 `private/analysis/ramlog-test-13/linux.txt`。第 13 次恢复后 26 个分区全部匹配，root 与 A 槽位正常。参考 [Linux ARM64 启动约定](https://docs.kernel.org/arch/arm64/booting.html) 和 [EFI Boot Stub 文档](https://docs.kernel.org/admin-guide/efi-stub.html)。

```bash
# 准备最小诊断与自动 RAM 日志。
python3 tools/prepare_handoff_probe.py
# 准备板级模块依赖、最小 initramfs 和原样内核载荷。
python3 tools/prepare_linux_modules.py
python3 tools/make_linux_initramfs.py
python3 tools/package_linux_payload.py
# 加 --raw 选择独立 ARM64 对照路径；默认 EFI-stub。
python3 tools/prepare_linux_profile.py --raw
bash tools/build_stage0.sh linux
python3 tools/package_stage0.py --profile linux --header-version 3
# 默认只检查，不重启设备。test-id 必须是未使用的编号。
python3 tools/ram_boot_stage0.py --profile linux --test-id 100
# 人在设备旁且已经授权本次实验后添加 --execute。
python3 tools/collect_uefi_ramlog.py --serial da9cb876 --test-id 100 --wait-seconds 240
```

原版含 SCM/TZ 的旧候选保留在 `artifacts/previous-candidate/`，其第 6 次原视频在 `private/analysis/video-frames/`。最小版第 7 次 BDS 证据在 `private/analysis/video-test7/`。每次镜像与 manifest 固定保存在 `artifacts/tests/stage0-test-N/`，硬件结果记录在 `private/analysis/stage0-test-N.json`；新编译的 manifest 默认仍是未验证候选状态。

第 14 次候选通过 `/dev/kmsg` 将 initramfs 的 PID 1、挂载表、模块加载结果与 USB 状态写入 ramoops。`collect_uefi_ramlog.py` 同时识别 UEFI 日志和新的 Linux 日志，`verify_boot_partitions.py` 只读校验 26 个启动分区。USB 观察脚本只操作 USB 标识明确为 SunUEFI-RAM 的 ACM gadget，不会连接其他设备串口。

第 14 次自动日志明确出现 `SUNUEFI_RAM_INIT BEGIN pid=1`；挂载只有 rootfs、proc、sysfs、tmpfs、devpts、configfs。65 个模块的 insmod 均返回成功，但驱动注册成功不等于硬件探测完成：AOSS/SCMI 与部分时钟/总线依赖尚未完成，最终 `USB_NO_CONTROLLER`。第 15 次候选增加 AOSS/SCMI 相关模块（共 71 个），并保留只读采集的 HWID 参数用于正确的板级 PHY/PMIC 选择。未复制或沿用原机指向 oops/blackbox 分区的日志参数。

第 16 次修正 HWID 的加载方式：plain insmod 不会像原机模块加载流程那样自动带入 `hwid.*` 命令行选项，因此直接传入已采集的模块参数。实际读回 `hwid_value=589824`、`project=9`、`build_adc=51282`、`project_adc=39406`。Linux 用户空间、模块加载、RAM 挂载与自动重启再次成功；USB 最终仍报告 `USB_NO_CONTROLLER`。当前待定位链路包括 CRM/RPMh 的 `-22` 探测失败、部分总线/IOMMU/PMIC glink 的延迟探测，尚未证明具体根因。相关公开驱动源码已按固定 MiCode 提交保存到 `upstream/reference-kernel/`，只作为参考，不宣称与预编译模块的构建提交完全相同。

已验证候选保存在 `artifacts/tests/stage0-test-16/`，其中 `hardware-result.json` 单独记录实机结果；镜像原 SHA-256 为 `f976b53c99aa197504036fd2e4f1cd55f69b9f88e02e9bc500806085c7da5668`。Linux 原始日志在 `private/analysis/ramlog-test-16/linux.txt`，分区核验在 `private/analysis/partition-verification-test-16.json`。本轮已停止实验重启，平板留在正常 Android。

## EDK2 外设与 simple-init 接入（进行中）

用户要求继续实现 GOP、USB、调试串口、触屏、UFS，再运行 simple-init。当前按 BigfootACA/simple-init 的 UEFI 应用实现，源码固定为 `3d66a6e78d519dd050fbebde4db6c5ac933f9aa4`，已完成 ARM64 EFI 编译；中文翻译和 Noto 简体中文字库内嵌，默认 `zh_CN.UTF-8`。应用约 26 MiB，从临时 boot v3 ramdisk 装载。独立 `pianoGuiPkg` 保留此前已验证的 Linux/diagnostic 目标。

GOP 已补上无效模式、空参数及 framebuffer 边界检查，加入像素填充/读回诊断。simple-init 还加入定时 GOP PNG 截图写入 console RAM，以及主机端校验/提取工具。`RamOnlySerialPortLib` 仅提供 RAM 调试输出，不能当作物理 UART 或 USB 串口完成。33 个原机相关 DXE 模块的原始依赖和哈希已清点；尚未执行 USB/触屏/UFS 硬件初始化。触屏已确认是 SPI Novatek NT36532，后续需要 SPI 传输和 Absolute Pointer 驱动。

GUI 候选不包含内置存储驱动，全部使用 fastboot 临时内存启动。第 17 次异常已恢复并读出日志，后续第 18／19 次已确认中文界面和放大后的布局。UFS 接入必须先具备源代码层面的写保护，不能只依赖应用层约定。

主要源码：`tools/build_simpleinit.sh`、`tools/prepare_simpleinit.py`、`tools/prepare_gui_profile.py`、`bootprofiles/uefi-app/`、`platforms/pianoGuiPkg/`。截图提取工具为 `tools/extract_ram_screenshot.py`；原机驱动依赖在 `private/analysis/native-driver-inventory.json`。

第 17 次用户照片中的异常地址 `0xD2754A5C`，减去应用加载基址 `0xD2725000` 后为 `0x2FA5C`，符号定位为截图回调 `Capture` 的入口。完整日志显示此前 simple-init 默认 Continue Boot 已返回 Aborted，应用已卸载；因此是遗留事件回调到释放的代码。修复退出时取消/关闭事件，诊断阶段延长菜单默认倒计时以便截图。GOP 实机 `BLT_ROUNDTRIP match=1`，字体加载成功；ConSplitter 暴露的 AbsolutePointer 不能当作物理触屏已可用。

第 18 次修复了截图事件退出清理，中文菜单由用户照片和 RAM PNG 确认；计时器成功自动恢复 Android。第 19 次将 GUI DPI 从 200 调整到 600，对应字号 24/16 → 72/64，按钮和图标随字号增大。PNG 传输出现一个不合法 Base64 字符，枚举字符后只有一个候选通过全部 PNG chunk CRC，恢复过程单独记录；它是校验恢复，不是修改画面。两次恢复后的 26 个启动分区均匹配。

第 20 次六个基础模块全部成功。第 21 次原机 ClockDxe、HALIOMMU、ICBDxe、DALTLMM、Qup、I2C、SPI、GpiDxe 等成功启动；QcomScmiDxe 的原始 DEPEX 未声明 ChipInfo，首轮顺序过早而返回 Not Found，已加显式依赖。PMIC 等待 SPMI，原机模块真实名称为 `SPMI`。第 22 次候选加入 SPMI、PMIC、ButtonsDxe 后在 PMIC 初始化中发生异常，已恢复并读回日志，未进入 Buttons。UFS 和 USB 物理控制尚未打通，候选仍未包含存储驱动。

第 22 次已恢复并取回完整日志：SPMI 本身 StartImage 成功；PmicDxe 初始化调用 SPMI，在 RVA `0x3758` 的 `ldr w9,[x3,x4]` 访问 `0x0C7B2008`，ESR `0x96000010` 为同步外部数据中止。该地址是写通道状态，寄存器和指令证明已经进入写事务路径，异常发生在该笔命令发送前。Buttons 与 simple-init 尚未运行。26 分区复核全部匹配。

`bootprofiles/uefi-app/PianoKeys.c` 提供独立按键候选，`prepare_gui_profile.py --keys` 与 `--foundation` 互斥，排除原机 PMIC/SPMI/Buttons 初始化。先核对运行时 DT 中控制器布局、EE 0 和 bus 0，再读取 v7 APID 映射并要求按键外设具备 EE 0 条目；只允许 SID 0 的 PON HLOS `0x1310` 和 SID 1 的 GPIO6 `0x8D10`。唯一 MMIO 写入是向 observer 通道提交一字节 EXT_READL，没有 PMIC 写命令或写 FIFO。短按电源松开后发 Enter，音量键映射标准扫描码，40 ms 去抖，超时/错误停止轮询，应用退出时取消事件和卸载接口。第 23 次实测版本为 `0x70020000`，896 个 APID，PON APID 581、GPIO6 APID 610，均为 EE 0。用户确认音量键移动菜单；RAM 日志同时确认音量扫描码 `0x80/0x81` 及短按电源 Enter `0x0D` 被 simple-init 读取。75 秒计时器正常恢复 Android，26 分区匹配。PNG 因 CRC 不匹配未采用为画面证据。

主机测试直接编译该 C 源码，用模拟 MMIO 检查地址白名单、读操作码、重复 APID 的所有者选择、拒绝与超时、按键去抖及长按电源不触发确认：

```bash
cc -std=gnu11 -fshort-wchar -ffunction-sections -fdata-sections \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  tools/test_readonly_keys.c -Wl,--gc-sections -o build/test-readonly-keys
build/test-readonly-keys
```

Simple-init 的 UEFI 键盘代码已提供音量上／下切换焦点、`SCAN_SUSPEND`／Enter 确认、Esc 返回的映射；小米实体电源键实际输出的扫描码仍需实机事件证明。`prepare_simpleinit.py` 固定加入 `PIANO_KEY_EVENT` 日志，避免重建丢失诊断补丁。原机模块准备工具支持重复 `--exclude MODULE`，恢复日志后可逐个隔离阻塞模块；仅允许排除当前组内模块，不会加入存储驱动。75 秒回调依赖 UEFI 计时器和事件调度，无法保证恢复关中断或提高 TPL 后的驱动死锁。

第 24 次候选延长诊断窗口至 120 秒，加入 `PIANO_MENU_SELECT/EXECUTE` 标记以区分菜单的选中与执行；同时给 Continue Boot 的直接 Boot Services Exit 路径补上截图事件取消，避免绕过 UefiMain 正常返回清理。第 24 次用户与 RAM 日志确认 `PIANO_MENU_SELECT simple-init` 和 `PIANO_MENU_EXECUTE simple-init`，工具主界面进入成功，120 秒自动恢复，26 分区匹配。

触屏当前只读采集确认：BOE 面板，lcd_id=1，NT36532 cascade，trim `0E0004326503`，固件版本 `0x13`、PID `0x59B0`。原机 `/odm/firmware` 的 BOE／CSOT 运行固件各 245760 字节以及匹配 `nt36532_touch.ko` 已读取并逐一核对设备端／电脑端 SHA-256，保存于 `private/captures/touch-test24/`。未执行触屏固件更新。原机模块保留的构建路径位于 vendor/xiaomi/proprietary/touch；公开 piano GKI 树的 touchscreen Makefile 不含该模块。NT36532 cascade 地址表已离线提取，但未完全标注字段，不能用于未审查的硬件写入。下一步验证 SPI 实际传输后接入 AbsolutePointer；ConSplitter 的虚拟触屏接口不代表物理触屏可用。

`tools/analyze_touch_module.py` 已按当前内核的 base BTF + 原机模块 split BTF 解码 `nvt_ts_mem_map`，并与 ELF 符号大小核对为 224 字节。真实 NT36532 cascade 的 EVENT_BUF_ADDR 为 `0x11C400`，single 型号则是 `0x125800`，不能混用。命名地址表在 `private/analysis/nt36532-layout.json`。原机 SPI 协议 GUID 为 `4c7ffd28-6a06-4425-9ee2-676ebc089683`，revision `0x10000`；三项回调的参数与返回 ABI 仍在核对，未按猜测调用。触屏所在 TOP_QUP_1_SE_2 的原机 UEFI 软件配置为 disabled，Android 的运行时设备树则启用 `spi@a88000`，SPI 初始化需要处理这处差异。

第 25 次 SPI 身份探测：仅在 XBL_DT RAM (`0x81A03000`) 中启用 QUP1/SE2，并将 active/sleep 两组 GPIO30..33 改为 GPIO40..43；主机测试比对整个 FDT，确认无其他改动。Native SPI Open instance 13 实机选中 QUP1/SE2，但 FIFO 接口检查失败，未发送芯片事务。第 26 次五个时钟均成功启用，在读取 `0xA8A008` 控制窗口时外部中止，尚未写入；计时器仍恢复 Android。第 27 次移除 SPI 不要求的 HALIOMMU/ICBDxe，以原机内核相同的 ARM64 SIP IO-read ABI 查询：服务可用，指定地址读返回 -2。该版没有直接访问受限窗口或 IO-write 调用，自动恢复正常，PNG 完整 CRC 通过。第 25/26/27 次后 26 个启动分区哈希均匹配；实际触屏未可用，未加载触摸固件，正在转向 DT 指定的 GPI DMA 通道与 IOMMU映射。

第 28 次在五项 QUP 时钟开启后手动启动 GpiDxe；接口 revision `0x10002` 安装成功，但 `fwload - ERR - fail!`，软件 ready flag=0。未注册 DMA 通道，未提交传输，未加载触屏固件；自动恢复，26 分区匹配。后续反汇编和第 30 次实测已纠正固件读取分支：当前 BootDevice 使用标准 BlockIO / 分区类型 GUID，`8d12919d-b55a-4324-ba9b-1d4cd7eccdfe` 只属于另一 BootDevice 分支。之后还依赖 SCM 接口 `77ed108d-8524-4b8b-9d2e-34987aecb9c1` 执行 QUP PAS peripheral 19 的 init/memsetup/auth/shutdown。原机 `qupfw_a/b` 各 131072 字节已只读捕获且核对双端哈希；当前 A 镜像 SHA-256 `b648516e1fde83a1b6a0d6a3c1bc3084adc63aecc34f7756c31850ab4ad4b4e9`。未启用 UFS 来读取固件。后续需从受校验的 RAM 载荷提供固件、完善受限 SCM/PAS 调用，再验证 DMA 地址映射。不能把 StartImage Success 或接口存在判定为传输可用。

第 29 次受限 RAM 读取接口安装但没有被调用。对加载器的进一步跟踪修正了“固件卷分支”推断：当前 BootDevice 分支是标准 GetBlkIoHandles，按 QUP 分区类型 GUID `21d1219f-2ed1-4ab4-930a-41a16ae75f7f` 选择 BlockIO。第 30 次新增 `PianoQupFwDisk.c`，仅把已校验的 128 KiB 固件副本呈现为标准 RAM 分区，所有 WriteBlocks 都直接返回 EFI_WRITE_PROTECTED（主机测试证明修改 ReadOnly 标志也不能开启写入）。原机 GPI 加载器确实读取完整 131072 字节；接口仍软件 ready=0，后续 SCM/PAS 认证加载及 DMA/IOMMU 映射尚未实现。该诊断要求 SCM 提供者不存在，以保证不进行认证、重置或固件编程。未加载触屏固件、未提交 DMA，120 秒自动恢复且 26 分区匹配。图形 RAM PNG 全部 CRC 通过，物理灰色/菜单状态仍待用户反馈，不能据此断言屏幕扫描输出已经修复。

## UEFI USB / fastboot 调试端（2026-10-04）

命令层在 `bootprofiles/uefi-app/PianoFastboot.c`，Qualcomm USB Device 适配层在 `PianoUsbDebug.c`。参考 EDK2 的 AndroidFastboot 与 USB transport，但采用自己的默认拒绝策略；不会调用上游 FlashPartition / ErasePartition / DoOemCommand，也没有存储、变量写入或任意内存写 API。

当前允许 `getvar:*` 的指定变量、`download:XXXXXXXX`（最大 64 MiB，仅 RAM）、`oem sha256`、`oem discard`、`oem log`、`reboot`、`continue`。`boot` 当前明确返回未实现，不会把未执行的交接报告为成功。所有其他命令包括 flash、erase、flashing unlock、set_active、任意 OEM 和特殊重启模式均拒绝。`continue` 结束 USB 调试服务，SimpleInit 继续运行。

传输适配器使用 fastboot 接口类 `ff/42/03`、bulk IN `0x81` / OUT `0x01`，预期 VID/PID `18d1:d00d`、序列号 `SunUEFI-piano`。发送队列、命令长度、RAM 下载长度、接收缓冲区和完成通知都有边界检查；收到最后一个 ACK 的传输完成通知后才执行冷重启。拔线清除 RAM 下载；若 DMA 中止失败则停服务并保留其缓冲区至诊断重启。当前尚未实测 USB 枚举，因此下列命令是接口接通后的用法，不能当作已经可用的 PC 调试证据：

```sh
fastboot -s SunUEFI-piano getvar product
fastboot -s SunUEFI-piano getvar storage-policy
fastboot -s SunUEFI-piano oem log
fastboot -s SunUEFI-piano oem sha256
fastboot -s SunUEFI-piano reboot
```

准备和构建（电脑端，不接触平板）：

```sh
.venv/bin/python tools/prepare_native_probe.py --group usb
.venv/bin/python tools/prepare_gui_profile.py --usb-debug --return-seconds 120
bash tools/test_usb_debug.sh
bash tools/build_stage0.sh gui
.venv/bin/python tools/package_stage0.py --profile gui --header-version 3
```

`--usb-debug` 与触屏 DMA 探测使用独立硬件实验组。UsbConfigDxe 的原始 DEPEX 未声明全部运行时依赖，现额外要求 HAL IOMMU 与 PMIC Version。不会用返回假成功的协议绕过依赖。`PianoProbeUsbPower` 独立读取五个已知 SID 的 PMIC revision 寄存器和 SID 7 中继器的四个 tuning 寄存器；revision 读取沿用内核的 EE0 observer 规则，由硬件判定读取权限，不把 write owner 条件误用于读取；它不是完整 PMIC 控制协议，也不会调节电源。

电脑测试执行实际命令层与传输源码，覆盖拒绝写入命令、非文本命令、无效长度、溢出和不完整 RAM 下载、SHA-256、发送失败、断线重连、队列溢出、无效 DMA 完成和 abort 失败；另有 AbsolutePointer 坐标 / 抬起转换与 SPMI 只读访问测试。USB 命令层和队列也通过 AddressSanitizer / UndefinedBehaviorSanitizer。实机第 31 次的日志和分区校验在 `private/analysis/ramlog-test-31/` 与 `partition-verification-test-31.json`。

## UFS 块层的早期离线实现（当前未启用）

用户早期要求加入 UFS 写入；当前最新要求已收敛为先完成 DMA / 只读 GPT milestone，实机写入关闭。此前离线实现 `PianoUfsBlockIo.c/.h` 使用真实 EDK2 ExtScsiPassThru ABI，提供 READ CAPACITY(16)、READ(16)、WRITE(16) + FUA、SYNCHRONIZE CACHE(10) 和 BlockIO 接口。默认拒绝写入；`PianoUfsEnableWrites` 只能打开明确给定的 LBA 范围，整个请求先检查范围再分块，两个 GPT 端点保留。它没有 FORMAT、UNMAP、RPMB、配置描述符或属性写入口；`PianoQupFwDisk` 仍是独立的不可写固件 RAM 盘。

`tools/build_ufs_block_layer.sh` 已生成 ARM64 COFF 对象 `artifacts/ufs-block/PianoUfsBlockIo.obj`，并执行实际源码的 RAM 模拟盘多块写入 / 读回 / flush / 边界 / 错误测试与 ASan/UBSan。**这只验证块层代码，不证明 piano 物理 UFS 控制器、PHY、SMMU / DMA 或文件系统已接通。尚未向实机 UFS 发块写入命令。** 后续实机写入需要明确的专用测试区域，保持现有系统和用户数据。

底层正在单独探测。`PianoUfsProbe` 核对同机设备树中的 HCI `0x1D84000`、寄存器窗口、SID `0x60`、ClockDxe ABI；只开启时钟/GDSC 并读取 CAP、VERSION、HCS、HCE 和 Qualcomm HW VERSION，不发 UIC、DMA、LUN 或数据块命令。未激活捕获的原机 UFSDxe。

## 自动异常恢复与构建校验

第 34 次实机已验证 `PianoFaultRecovery`：注册同步与 SError 回调，不改变 IRQ/FIQ；未定义指令触发后，把 PC/ESR/FAR/SPSR 写入保留 RAM，再冷重启回 Android。26 个启动相关分区匹配。普通 ASSERT 死循环未必进入异常向量，不能据此声称所有卡死都能自动恢复。

第 35 次独立 `PianoPmicMetadata` DXE 驱动实际读取 SID 0/6/7 的型号与版本，按捕获模块的原始两回调 ABI 提供 PMIC Version，代码独立驻留。UsbConfigDxe 已实际启动；下一层失败为缺少 USB NPA 电源节点、SPMI / I2C 中继器通路与 USB Power Control。175 秒电脑端枚举观察没有出现 `SunUEFI-piano`，因此 USB fastboot 尚未可用。

第 36 次因构建流程错误重复运行了第 35 次镜像，没有进行 UFS 探测；镜像 SHA 已核对相同，恢复后 26 分区匹配。现 `build_integrity.py` 在构建开始时废止旧成功标记，构建完成后核对输入与输出；封装和 RAM 启动都要求同一个成功构建编号。失败/中断构建、编译期间或之后改源、产物篡改和旧封装均通过电脑测试证明会被拒绝。第 37 次才是带新构建编号的独立 UFS 探测。

第 37 次在首个 UFS CAP 读取出现 ESR=0x96000007（level-3 translation fault），由自动恢复回 Android；内存表确实遗漏该区域。第 38 次只添加 HCI 的 0x3000 字节设备映射，其他映射逐字比较未改变，读寄存器成功。HCI 4.0 / Qualcomm 6.1 仍需平台 PHY 与 SMMU / DMA、命令列表和 LUN 的实际验证；控制器存在和 HCE=1 不能代替读盘证据。

## 当前 DMA milestone（2026-10-05）

当前范围收敛到统一 DMA/SMMU → UFS NOP/QUERY → LUN/容量 → 只读 LBA/GPT。完成真实读取之前，禁止实机 UFS 块写入；后续首次写入必须使用明确专用区域，执行原块备份、测试写入、FUA/同步、读回、恢复与再次校验。USB 只做枚举，触屏与 USB 后续统一复用 DMA 层，暂不扩展上层。

`PianoDma` 提供自有页分配（限定 DXE_Heap）、CPU 地址 AT 翻译校验、WB 属性检查、32/64 位地址约束、cache clean/invalidate、映射/提交/完成/解除映射/释放生命周期。没有后端时拒绝假设 identity mapping；无法确认 DMA 停止时隔离内存并保留到重启。

39/40 次只读 SMMU 快照：apps SMMUv500 ID0=4C017E7F、ID1=60000053、ID2=5111，127 SMR、83 context banks、4 KiB page、默认未匹配流为 fault；加载基础驱动前后不变。UFS SID60 的原槽0无 valid、S2CR=fault；USB SID40 的槽3亦无 valid；显示 SID800/mask2 的槽2保留有效。

43 次已经实测只给 UFS_MEM 创建自有上下文/页表，CB0、MAIR0=FF、TCR=802519、TCR2=38061（含 RES1 读值），PA D3CA1000 → IOVA 40000000 的软件页表查询匹配，其他流未改变，映射/解除映射/销毁成功。**这不是实际设备 DMA 成功。**

44 次首次真实 NOP 门铃没有完成；无法确认停止时保留缓冲区并自动重启，26 分区匹配。没有执行 QUERY、LUN、容量、LBA 或 GPT。45 次只读确认 MEM_CFG=0、原机处于单门铃模式。46/47 次在 Hibern8 exit 后真实 NOP DMA 成功；Descriptor FF。48 次读取 bCurrentPowerMode=33，确认存储设备仍为 PowerDown。49 次无数据恢复 Active 后，89 字节 Device Descriptor、6 个 LUN / 容量成功。50 次全部 6 份 GPT 的 header / array CRC 成功，解除 SMMU 后读回也通过。Android 只读对照与 UEFI 全部一致，26 分区匹配。54 / 55 次同一镜像复测通过：每轮 37 次命令、21 个元数据块、220 条完整 CRC 校验记录，Android 对照和 26 分区校验一致。原始日志损坏仍会出现，镜像回收拒绝损坏副本，保留原始捕获。

内存范围、SID、API 生命周期、恢复策略和证据位置见 [DMA / SMMU milestone 状态](docs/dma-smmu-milestone.md)。可复用主机检查为 `bash tools/test_dma_foundation.sh`；Android 元数据对照为 `tools/compare_ufs_gpt_android.py`，只有只读命令。

已验证的 60 秒自动返回 Android 的 RAM 诊断镜像和证据清单：`artifacts/dma-milestone/`。工具 `check_dma_log.py` 核对核心 DMA 记录，`compare_ufs_gpt_android.py` 只读对照 GPT，`seal_dma_milestone.py` 固化两轮完整验证后的产物。

新增待实机候选已完成完整EDK2构建：受保护[键盘SE6只读快照与高RAM只读元数据](docs/readonly-device-profiles.md)，以及接入标准boot命令/真实IN ACK/Controller退休/carrier/Load+Start的[小EFI返回应用](docs/ram-boot-probe-profile.md)。默认关闭、当前容量64MiB，首个boot profile仅允许固定probe SHA；尚未实机启动，不替代第89次联合关闭故障的CRC取证。
