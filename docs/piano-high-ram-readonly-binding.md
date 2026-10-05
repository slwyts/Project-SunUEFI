# 下一次high-RAM AT-only profile的实际绑定审核

**2026-10-05后续实证：**第91次已回收四个高地址 `A00000000`、`A00001000`、`A3FFFF000`、`A3FFFFFFF` 的实际 AT 记录，均为 `PAR=80B`、`translated=0`，没有解引用目标内存。当前产品不能使用该1GiB arena。新 Android 只读快照位于 `private/analysis/android-memory-2026-10-05/`；虽然整份 DTB 摘要变化，16个非零memory tuples、47个fixed reservations、14个dynamic constraints、zero placeholders与FDT memreserve均和原捕获完全一致，比较结果另存 `dt-memory-comparison.json`。Android高RAM可用不能替代UEFI阶段所有权证明。后续工作使用唯一产品核心的DDR/HOB/MMU/GCD接入，不再把本文件早期独立诊断profile建议当成最终功能分支。

本轮只读审查，没有改helper、prepare、当前源码/表、设备或build。目标为下一次独立采集AT/EFI/GCD；不apply high prototype、不pattern、不公告1GiB容量。现helper冻结SHA仍：C `8d6826d417791d80b14dd54a422afdaac5ae80d6b0d6efd20735f0a0fcb8aa4d`、H `74542850cbd89943073fc4e1fc1545503aa7307f5d9986707321c4c235ffd106`。

## MemoryAttribute producer存在于源码，但下一次不调用Get

当前Mu-Silicium HEAD66e7bd1e7bcb757d4b28629bd6409d7209d3b242、Mu_Basecore HEADbb557081f80f4883ed832e34ab36bdca6ede1e10。pianoProbe/pianoGui FDF使用`ArmPkg/Drivers/CpuDxe/CpuDxe.inf`，不是X64 CpuDxe；该INF编译MemoryAttribute.c。已构建pianoGui ArmCpuDxe.efi SHA `d3e94ebd7187e7b01007b22140942606c34c7fe8113d21e5c191fd5b7790fad3`，map含GetMemoryAttributes RVA377c、GetMemoryRegionRec3170/GetMemoryRegion32c4。这是本机build artifact证据，不是协议实机installed/read证明。

源码`ArmPkg/Drivers/CpuDxe/CpuDxe.c:447`仅在`gDxeMps.InstallMemoryAttributeProtocol`为TRUE时安装标准GUID。`DxeMemoryProtectionHobLib.c:268..298`从policy GUID HOB取得设置；缺HOB或版本无效会清零全部settings，Install字段为FALSE。现Silicium SEC新建HOB list、MemoryPeim路径没有查到该policy HOB producer；已构建使用QcomPkg PlatformPeiLib，其PlatformPei.c:76..139构建InfoBlk/Shim/Prodmode/Scheduler/FvList/DTBExtension HOB和FV HOB，也没有该policy。native EnvDxe等运行期影响未捕获，因此只推断“未显式配置时预期未安装”，不能声称实际一定不存在。

实际已构建AutoGen的`PcdRemapUnusedMemoryNx=TRUE`是**另一项独立设置**，不是InstallMemoryAttributeProtocol；不能用它作为protocol已安装证据。

GetMemoryAttributes的ABI和行为可以从源码核对：`MemoryAttribute.c:74`调用`GetMemoryRegion`，union/intersection不一致或region失败返回NO_MAPPING，成功只返回RP/RO/XP（109..110），不返回WB等cache属性。但`AArch64/Mmu.c:458`直接解引用`*BlockEntry`，462..465取下级table OA并递归读，没有helper的EFI owner、low-table范围、AT/IPS guard。它是内部bare PTE walker，**不能在physical-regime/下级页表可信性未知时作为安全替代**。

因此下一次只Locate并记录status/pointer是否存在，不调用Get、不改变Mu policy去安装它；绑定：

```c
Env.MemoryAttribute = NULL;
Env.MemoryAttributeProviderVerified = FALSE;
Env.ClassicEl1Stage1PhysicalContractVerified = FALSE;
Env.LowTablePagesAndRecoveryVerified = FALSE;
Env.ReadTableWord = NULL;
Env.AtWrite = NULL;
```

Root已确认该方案，结论也已传给Pogo probe agent，避免其首次MMIO验证间接触发bare PTE walk。`MemoryAttribute.c` SHA89e3e6c1248783b1ef7b40685fcda25fe580eef981907205f9bf6bb7d5ed499e，`AArch64/Mmu.c` SHA173073a5a9e827be71aa0222e7af26c6122d159eb314f79bfcc07c6c95d3b533。

## 已有CPU、GCD和异常入口

helper已有实际AArch64 `PianoHighRamArchitectureState`与`PianoHighRamArchitectureAtRead`，可直接绑定，不需从旧DMA allocator派生新高DMA路径。它检查当前EL1/classic4K/IPS格式，AT只返回PAR，保存恢复PAR/DAIF；regime是否将stage1输出映射为实际physical仍未确认，必须保留FALSE。不要尝试在EL1读取未知EL2/HCR来“证明无stage2”。

`SiliciumPkg.dsc.inc:229`已提供DxeServicesTableLib实例。其constructor从SystemTable配置表取得gEfiDxeServicesTableGuid；Root派生RamApp只需将该LibraryClass纳入INF，使用gDS->GetMemorySpaceDescriptor。gBS->GetMemoryMap使用STATIC UINT64-aligned预分配scratch，helper限定一次调用；所有status精确EFI_SUCCESS才接受，其他结果原样日志并unknown。EFI descriptor attrs、GCD Capabilities、GCD Attributes分别输出，不当作同一含义。

现`PianoStartFaultRecovery()`、`PianoStopFaultRecovery()`可重用，需纳入PianoFaultRecovery.c及既有Cpu/Runtime/ArmSmc/Serial/Print依赖。它注册sync+SError handler，记录PC/ESR/FAR/SPSR后cold reset；递归fault跳过诊断hook。它不是可返回的safe-load wrapper，不能给bare memory read虚构EFI_STATUS恢复。若编译PIANO_USB_UFS_FETCH，异常路径反而CpuDeadLoop保留DMA所有权；因此本次必须为独立profile，不能复用combined fetch宏或其HAL/DMA初始化。

## TTBR root来源与保护边界

归档test88 CPU trace：EL1、TCR480803514、TTBR0 D7FFF000。52条实际EFI map中index42 type4 BootServicesData覆盖`D7AA6000..D8000000`，包含root整页；attrs100F。这仅属于test88阶段，下一次必须重新捕获。

实际源码对应：SEC在DXE heap建HOB arena；ArmMmuLibCore.c:834用AllocatePages(1)建root；PrePiMemoryAllocationLib:105以BootServicesData分配，并由77的BuildMemoryAllocationHob记录完整页，返回从arena顶部下降的PA。该路径解释归档root占用类型，不能替代运行期页表内容或chain owner证明。

type4证明占用分类，不证明root/下级table实际RO/AF/XN/cache。CPU driver的PcdRemapUnusedMemoryNx为TRUE，会重映射unused memory；其他DXE保护又受policy控制。EFI100F也不是CPU PTE读回。当前未回收chain页/AT/PAR/PTE保护证据，所以所有table-read验证字段继续FALSE。

可额外只AT当前TTBR0的低控制地址并查其本次EFI/GCD占用分类，得到raw PAR/cache/readability证据；不得据单个root AT成功开启下级walk或把S1输出当物理owner证明。

## 最小可构建的派生绑定

推荐在独立派生RamApp中绑定现helper，默认profile仍不启用。实际当前CLANGPDB GNUmakefile使用clang、`-target aarch64-unknown-windows-msvc`和`-D`参数；Root应在**派生module build options**明确加probe=1、pattern=0，不改冻结header默认值。INF additions仅PianoHighRamProbe.c/.h、PianoFaultRecovery.c，LibraryClasses加DxeServicesTableLib和既有fault依赖；标准MemoryAttribute GUID可仅供Locate inventory，不需加新的producer。

建议调用顺序：

1. 确认既有独立return timer已创建，启动fault recovery并要求exact EFI_SUCCESS；不用synthetic fault-test，不加载native groups。
2. 使用gBS/gDS、static scratch、ArchitectureState/AtRead和RamOnly serial logger建立Env；readonly ExplicitEnable=TRUE，pattern永远0，上一节五个危险入口字段保持FALSE/NULL。
3. `RowBudget=1`调用PianoHighRamProbe取得fresh CPU/map、首点A00000000、GCD与预期unknown/partial。NOT_READY是本次诊断预期，不能将其当成功映射，也不需要为“过gate”修改FALSE字段。
4. 利用已有ArchitectureAtRead接口，额外固定三个点：A20000000、A3FFFF000、A3FFFFFFF。各自独立查询GCD，并从本次验证成功的EFI map snapshot只读取descriptor metadata；如果snapshot失败，记录unknown而不扫无效buffer。总计start/middle/last-page/last-byte四个高点，不以512行遍历头2MiB代替边界证据。
5. 汇总marker明确samples4、target_dereferenced0、table_walk0、ownership_verified0、full_1GiB_verified0；停在已有timer等待/独立返回路径，不再LoadImage SimpleInit/OS、ProbeGop roundtrip、初始化USB/UFS/键盘/PMC/DMA或公布高容量。

不需要新helper API。四点adapter只负责现有函数和EFI/GCD接口的接线/日志；Root负责实现并构建派生profile，本轮没有执行prepare或build。

这样下一次能区分high地址AT translation/fault、EFI/GCD advertised coverage和stage1属性状态，保留physical-regime/owner未知。它不会建立高映射、读高内容或接通pattern。只有Root后续独立审查过真实table chain和所有权后，才另评估occupied1GiB/CPU arena步骤。
