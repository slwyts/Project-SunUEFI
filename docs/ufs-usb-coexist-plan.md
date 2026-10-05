# UFS + USB readonly fastboot coexist integration

2026-10-05：本次只审计/修复 `PianoFastbootBlockRead.c/.h` 并增加 host tests，没有修改 Dwc3/Controller 注册、prepare、UFS busy 文件或设备。USB test82 的标准 RAM/log roundtrip 是已测基线；本文件中的 combined owner、setter、finalizer 均是待实施建议，不代表同时运行 UFS/USB 已实测。

## 真实 readonly EFI bridge 的边界与修复

桥只接受 exact `Vendor(A8675600-87D0-4A29-9B40-600000000001) / UFS(Pun0,Lun0..5) / HD(GPT) / END_ENTIRE` path。每个 node 长度必须等于真实结构，插入/扩展节点、END_INSTANCE、错误 MBR/signature type、PartitionNumber0 都拒绝。DevicePath 协议只有 pointer，没有 allocator extent：END_ENTIRE 之后的 allocation padding 不属于 path，不能据此声称检测了不可见的物理 trailing bytes；测试覆盖的是 path 内新增节点、长度和 termination。

HD 的 PartitionStart/PartitionSize/Signature 现在分别核对 GPT StartingLBA、EndingLBA-StartingLBA+1、UniquePartitionGUID；容量同时核对 BlockIO.LastBlock+1，按 4096-byte blocks 检查 overflow。Type/Unique GUID 不得为零，Start不得为0。固定 Mu 的 `MdeModulePkg/Universal/Disk/PartitionDxe/Gpt.c:249–258` 正是以这些 GPT 字段构造 HD node，是本地源码依据。

接受的 BlockIO 必须为真 LogicalPartition、ReadOnly、MediaPresent、BlockSize4096、IoAlign0/1，且存在有效 Revision/ReadBlocks；错误 alignment 在 Init 就拒绝，不能先 advertise Ready 再发现无能力读取。GPT name 只接受最多36字符的 ASCII `[A-Za-z0-9_.-]`，没有 case folding 或 suffix alias；第一个 NUL 之后非零字符拒绝。完全36字符的合法 GPT name 仍可使用。

每次 Ready/Info/ReadBlocks 重查真实 handle 的 BlockIO、PartitionInfo、path、MediaId、容量、ReadOnly、alignment 与 ReadBlocks pointer。成功 read 之后再查一次；read过程中 revoke / MediaId改变时返回失败，fetch协议不会发送DATA。重复 exact name 返回 EFI_NO_MAPPING，不能猜测选择 LUN。Stop 使 callback Ready/Info/Read 失效；opaque cookie 单调递增，Stop→Init同名同容量同槽位也不会恢复旧 token。counter耗尽拒绝，绝不wrap重用。定位接口的空/损坏结果以及初始化失败会释放handle array/清除未发布metadata。

`tools/test_fastboot_block_read.c` 编译实际 bridge + actual fastboot command layer，使用 EFI mock。覆盖正确 MediaId/relative LBA、跨4K fetch的两次完整块读取、exact name/ambiguity、HD/GPT/Unique GUID、node/END、alignment、只读/函数/protocol/媒体撤销、read期间撤销、Stop/reinit stale cookie、160项上限、全长36字符名、warning/error normalization与pool清理。WriteBlocks mock 一旦调用就失败；所有测试确认0写。已通过 ASan/UBSan/leak detection。

统一 host 入口：

```sh
bash tools/test_usb_fastboot.sh
```

桥没有进行 BlockIO/设备实测，也未被注册到 Dwc3 的 private mFastboot。首次 live acceptance 仍应只用 exact `xbl_config_a`，与已存在的 PC 原始备份核对长度/SHA；`max-fetch-size` / `partition-size:<name>` 只有真实 Ready 后才可成功。

## SMMU 真实源码与 native 依据

`PianoOwnedSmmu` 每个 context 单独拥有 Domain、TableMemory、PageTable、Used[4096]、Mapping[64]、Before/After snapshot。UFS static mContext/mDevice 与 USB static mUsbContext/mUsbDevice 分离。每个 arena 可以使用相同的 0x40000000..0x40ffffff IOVA，但必须由 SID60/USB SID40 的不同 stage-1 CB 和不同 TTBR root 解释。不能共享 mapping token 或把 UFS buffer 的 DeviceAddress直接交给USB。

`build/HALIOMMU.disasm` 中 Create RVA1644为每个Domain分配0x40-byte对象；Attach RVA1744维护该Domain的resource list；RVA1940–19a8从0开始扫描可用CB、1aa4把选定bank写入domain-resource field，1aa8之后以该bank配置寄存器。因此接口并非强制CB0。`PianoOwnedSmmu.c` 把Type0/TTBR配置交给HAL，再从实际S2CR取得ContextBank，未写死CB编号。这个证据只支持多domain可行，仍需combined实机验证分配出的两个bank。

现有 Open/Close 的“其他stream未改变”主要比较 RawSMR/RawS2CR。它**不能独自证明其他CB的TTBR/TCR/MAIR没有被修改**。最小combined验收应增加：

1. 打开UFS并保存实际SID60的CB/root/config；打开USB后要求两SID均translate、CB不同、TTBR root不同，UFS配置仍等于原值。
2. UFS与USB各至少一个live mapping，同时记录各自PA、IOVA、SID、CB、root，使用各自PageTable进行software translation。允许IOVA相同，CPU PA / owner token必须不同。
3. USB bulk持续可响应时读取exact `xbl_config_a`，标准CLI fetch到PC并核对既有原始SHA；之后再次读取RAM/log upload，防止把“两个独立测试都成功”误认为共存。
4. USB Close/Destroy后SID40消失、SID60依然存在且CB/root/config完全不变；再由UFS真实read证明它仍可工作。最后关闭UFS后SID60消失，所有其他stream及其相关CB寄存器保持 baseline。

## 最小 setter / result API 建议（未实现）

建议将现有USB-only wrapper保留，新增一个**返回 action、不执行 ResetSystem**的combined entry，而非新增全局外部指向private fastboot state：

```c
typedef enum { PianoUsbContinue, PianoUsbColdReboot } PIANO_USB_EXIT_ACTION;
EFI_STATUS PianoUsbControllerRunWithStorage(
  CONST VOID *Fdt,
  CONST PIANO_FB_STORAGE *Storage,
  PIANO_USB_EXIT_ACTION *Action
);
```

这个entry复制Storage callback结构到run的状态；Context本身由上层UFS owner保证生命周期。只有Idle入口可以绑定，禁止active DMA时rebind；Init没有Ready时不advertise storage。Device 内仍使用已有 `PianoFastbootSetStorage()`，USB层只负责自己的Halt/DMA/session/Close/clocks，成功后返回已ACK的ColdReboot请求。Current `PianoUsbControllerExperiment` 可以作为USB-only wrapper使用这一结果立即reset，保留83行为；combined caller必须拿到action后处理剩余owner。

单次combined flow的必要顺序是：Open UFS → Connect Partition/DiskIo等readonly children → Init BlockRead bridge → Open/Run USB。退出先停止接收/排队命令并drain或haltUSB，撤销backend（SetStorage NULL + BlockReadStop），Close USB → Stop/Close UFS。USB After/Before snapshot包含已打开的UFS，故不能先Close UFS；也不能放宽snapshot比较以掩盖顺序错误。

## 全owner finalizer 与 OS Exit

建议上层统一执行 `EFI_STATUS PianoDebugShutdownAll(Action)`：确认USB清理成功后撤销桥，执行UFS完整normal Stop（先给予child drivers Disconnect/Stop，在media仍可用时完成flush，再Halt/回收/owned Close/clock release），所有步骤精确EFI_SUCCESS才最终cold reset。任一步未知quiescence、quarantine或release失败都报告保留状态并拒绝假clean；继续使用已有fail-stop机制。不能在USB Controller reset时忽略仍live的UFS。

目前 `PianoUfsBlockIoStop()` / `PianoUfsStopClocks()` 返回VOID，不能只凭“函数已调用”在上层宣称所有release成功。未来应提供checked shutdown结果，包含doorbell=0、TR/TM engine stop、buffer/table retained数量、detach verification和clock release状态。`Cleanup()` 在释放owned context后还会恢复inherited Run bits；combined normal reboot / OS handoff需要明确选择是否保持HALT，不能把恢复后的Run1等同于全engine停止。

后台模式应把USB/UFS owner登记到同一个shutdown协调器，并由application级work loop运行fetch/GOP/分配；高TPL通知只置work/stop标志。当前UFS ServiceRead使用RaiseTPL(TPL_CALLBACK)，不能从更高TPL直接调用。不要在定时器或EBS callback里做完整BlockIO读取、GOP截图或Boot Services协议 teardown。

正常进入OS前应在Boot Services仍可用时完成所有owner normal Stop，并取得最后memory map。若无法正常关闭而必须由EBS fence保留，USB也必须具备Halt/active-DMA retirement与预先Reserved的全部buffer/table；当前UFS EBS handler只处理UFS，shared DMA没有全owner registry，USB bulk buffer默认非Reserved，不能用UFS的一次成功Halt替代USB关闭。EBS callback只允许allocation-free halt/cache/retain，不能native HAL detach/free或改变final map。当前83的前台USB-only实验不存在这种后台EBS实现。

本轮没有修改任何setter/注册/finalizer源、UFS源、Dwc3/Controller或设备状态。这些集成条件仍应由后续combined profile和实机日志逐项证明。
