# Piano high-memory probe：默认关闭的实际C helper

本轮交付`uefi/core/PianoHighRamProbe.c/.h`、真实C host测试和AArch64指令审查。没有加入prepare/profile/INF、启动硬件测试、改88 memorymaps/kernel/pins或扩大fastboot公告；当前默认两个宏均0。Root负责后续集成和设备验收，当前status文档不在本轮修改范围。

```c
#define PIANO_HIGH_RAM_PROBE_EXPERIMENT 0
#define PIANO_HIGH_RAM_PATTERN_EXPERIMENT 0
```

probe覆盖已审查`A00000000..A40000000`候选。两个macro、运行时ExplicitEnable及各验证合同分开；默认编译连AT指令和callback调用都没有。pattern macro即使单独为1，probe macro0仍拒绝。每个宏只允许0或1。

## 只读translation与属性路径

`PianoHighRamProbe()`捕获CurrentEL/SCTLR/TCR/TTBR0/MAIR，并尝试一次caller预分配buffer的GetMemoryMap；没有AllocatePool/resize/retry。最多256个EFI descriptors，stride40..256、8byte倍数、version1、范围不overflow且互不重叠。缺失、BUFFER_TOO_SMALL、格式异常和warning状态均保持unknown。不会按旧snapshot或部分buffer继续假定EFI所有权。

每个segment调用AT S1E1R；原生wrapper短暂mask DAIF，保存PAR_EL1、执行AT/ISB、读取PAR后恢复原PAR与DAIF。它不读目标内容、不写TCR/TTBR/MAIR/SCTLR、没有table/TLB/mapping更新。[Arm官方memory-management说明](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf)支持AT将翻译/属性或fault结果写入PAR，而不是对目标作普通内存访问。AT结果可能是stage1输出；是否等于物理PA还需独立regime契约，不能由EL1/TCR alone认定。

首版严格支持当前EL1、4KiB、classic非LPA2格式。拒绝EL2/EL3、MMU off、非little-endian table regime、TG0其他granule、EPD0、HA/HD或DS、未知IPS。原生AT wrapper本身也做这些检查。TTBR必须是当前source使用的4KiB对齐root；ASID不当作地址。IPS分别按32/36/40/42/44/48位计算PA上界，TTBR、下级table OA、leaf完整block区间和PAR OA超出上界时拒绝/unknown，不能只mask48位后接受。

GCD查询只用`GetMemorySpaceDescriptor`，分别记录Capabilities和Attributes及ImageHandle；GetMemoryMap attributes也独立记录，不能将capability位集合当作CPU实际cache。caller可提供已确认实现/ABI的标准`EFI_MEMORY_ATTRIBUTE_PROTOCOL`，只调用GetMemoryAttributes查询sample4KiB的RP/RO/XP，没有Set/Clear。provider不存在/未验证/NO_MAPPING/warning均报unknown，不自动Locate一个未验收provider。

## 受限页表walk与readback

walk最多4层，支持L1 1GiB block、L2 2MiB block、L3 4KiB page；记录每层raw entry、entry VA、table PAR、level、最终OA/block size、MAIR index/byte、SH/AP/AF/PXN/UXN及层级APTable/PXNTable/UXNTable影响。HPD0存在时按其含义不继承hierarchical限制。L0 block、未知descriptor格式、未对齐OA、IPS之外OA拒绝。对权限的解码遵循[Arm官方memory-model中的descriptor和hierarchical attributes](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20memory%20model%20guide.pdf?revision=58b1dd0a-3800-4218-b21a-f95a0332034c)。没有扩展权限/MAIR2格式的猜测解析。

helper不直接解引用任何页表指针。交给caller `ReadTableWord`的地址必须先通过：

1. 独立确认classic EL1 stage1→physical合同及low-table/recovery合同，两个字段必须精确TRUE。
2. word在`BD980000..D8000000`已知低heap候选内，8byte对齐；排除heap起始adspslpi冲突及`D4E23000..D5100000` hwfence冲突。
3. 本次EFI snapshot完整覆盖该4KiB table页，type必须BootServicesData或LoaderData，含WB能力。Conventional、Runtime或Reserved不能因此自动变成可读table页。
4. table word自身AT成功、正常WB PAR属性、identity地址且PA位宽合法。

然后才允许一次受保护table-word读取。backend必须有已经安装/验收的fault recovery并保持有界；本文件不提供naked volatile dereference。下级table pointer也重新验证所有条件；不会顺着PTE指针去读高RAM。低heap范围只是额外拒绝边界，不能代替阶段owner验证字段。

最终比较PTE OA、PAR OA和预期identity PA；比较MAIR/SH，并检查AF、WB/inner shareable及XN。不同、缺失或extended/DBM格式为unknown。每个segment之后再捕获CPU translation state，变化即停止。segment按mapping granule及所有EFI descriptor/GCD边界切分，EFI洞不会继承前段属性。缺walk时只前进4KiB；RowBudget1..512，耗尽是partial/unknown，不假称覆盖整个1GiB。

## 日志与结果含义

caller提供有界Log sink；helper以小于256bytes的行输出：

- `PIANO_HIGH_RAM_AT`：VA/end、raw PAR、PA-from-PAR、status、unknown bitmask。
- `PIANO_HIGH_RAM_EFI` / `GCD`：descriptor范围、type、capabilities/current attributes、owner和原始status。
- `PIANO_HIGH_RAM_MEMORY_ATTRIBUTE`：标准protocol的raw permissions、status与sample_bytes=4096。
- `PIANO_HIGH_RAM_WALK` / `PTE`：walk PA、block bytes、leaf/parent values、MAIR/AP以及每层entry/readback。
- `PIANO_HIGH_RAM_UNKNOWN reason=...`：明确缺失/拒绝条件，不把protocol存在或AT成功写成真实RAM读取。

**所有callback输出只在status精确EFI_SUCCESS时接收。** warnings即使填满正常数据，也不能成为proof；raw status仍在日志中。public probe只有metadata完整一致时EFI_SUCCESS，否则NOT_READY/UNSUPPORTED；`TranslationMetadataConsistent`仅表示这些只读证据互相一致。`OwnershipVerified`、`PatternPermitted`、`TargetMemoryRead`和`TargetMemoryWritten`在此入口始终FALSE，且每个segment明确记录physical phase ownership unknown。`Complete`只代表按已读granule/descriptor遍历了整个窗口，不证明物理DDR可用或实际数据已读。

## 将来的4KiB保存/恢复接口

`PianoHighRamPattern4K()`目前无实际read/write/cache backend，宏默认0。它不会因上述metadata一致而自动允许写；未来需要单独明确enable、exclusive阶段ownership、DMA/其他CPU quiescence、cache/alias、fault/restoration以及两页独立低内存scratch验证，全部字段精确TRUE。还要重新捕获当前map/regime、确认完整4KiB type2/WB EFI区、匹配已分配GCD owner、实际PTE与AT R/W identity/permissions和已验证MemoryAttribute查询。saved/work buffers不得重叠或位于高候选，target只能是候选中的aligned4KiB。

只有全部通过后才save原页，write一个有界pattern、调用cache backend、readcompare，再restore原页/cache/readcompare。backend必须按其cache/alias合同实现clean/必要invalidation与完成屏障；helper不自实现cache指令，不凭一次同CPU比较声称DRAM外部读写通过。synthetic clean/read模型仅用于验证控制与恢复顺序。

任何pattern写尝试，即使返回error/warning且可能只改了部分页，也会恰好尝试一次restore。恢复失败返回ABORTED并保留SavedPage给owner恢复，停止进一步动作；没有无限retry/reset或DMA配置。所有warning public statuses规范化为EFI_DEVICE_ERROR，`RawStatus`/`RestoreRawStatus`保留原值；没有把warning当成功的出口。

## 集成与host验证出口

Root未来集成必须明确提供：预分配map buffer、gBS/gDS和已确认的MemoryAttribute provider、ArchitectureState/AtRead、受保护ReadTableWord及Log。运行时enable默认FALSE，两个regime/low-table验证字段默认FALSE。缺字段时可保留AT/metadata诊断，但不能伪造TRUE来让table读取过gate。pattern与当前只读profile始终分开，不能改旧DeviceDmaAllocator、增加GPI/SMMU映射或自动apply offline high table。

```sh
python3 -m unittest discover -s tests/unit -p test_piano_high_ram_probe.py -v
```

实际测试是**2个Python unittest methods、4次真实C host gate组合执行、3组AArch64 object/指令检查**。C使用真实Mu UEFI/PI/protocol headers与ASan/UBSan，覆盖default-off零callback、1GiB/2MiB/4KiB walk、EFI洞边界/partial budget、translation fault/identity mismatch、low表owner/guard、cache/hierarchical权限、state变化，以及State/map/AT/table/GCD/MemoryAttribute六类“warning但输出完整”的拒绝。额外检查IPS44之外TTBR/下级table OA/leaf OA/PAR OA不变proof。

未来pattern-on只在synthetic backend执行保存/比较/恢复、partial write、cache失败、compare失败、restore失败、全部ownership gate拒绝和warning规范化；不是硬件DRAM测试。AArch64实际编译/反汇编确认默认off没有AT/BLR，readonly-on只有S1E1R、PAR/DAIF保存恢复，S1E1W仅在两个macro均1时存在；没有修改TCR/TTBR/MAIR/SCTLR、allocator/属性写入或DMA符号。当前memorymaps/prepare/kernel/pins和Root待回收设备状态均未改。
