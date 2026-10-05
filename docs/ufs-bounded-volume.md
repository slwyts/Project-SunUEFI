# 限定 UFS gap 的一次性 BlockIO/FAT 实验后端

本版本只有电脑端 actual C/ABI/ASan/UBSan/AArch64 语法检查。Root 已报告 test79 全只读 preflight、test80 固定首块 WRITE(FUA)/SYNC/read/restore/SYNC/read 成功，以及 Android 后26启动分区与 capture2 九份 blob 全匹配；这些是原固定块 transport 的实机证据。本 bounded provider 未进行固件准备、构建、设备操作、格式化或恢复写，不把14MiB卷描述为已验证或持久卷。

## 固定范围与默认关闭

`PIANO_UFS_BOUNDED_VOLUME` 默认不定义，必须显式与 `PIANO_UFS_BLOCKIO` 一起定义。它互斥固定首块 test，禁止 `PIANO_UFS_FILESYSTEMS` 的原父LUN probe 和 Setup；Shell可以另行链接。此阶段未改 `prepare_gui_profile.py` 或原 test80 文档。

- 唯一 volume logical LBA0–3583 → physical LUN4 LBA375040–378623。
- BlockSize4096、LastBlock3583、IoAlign1；不做512-byte模拟/RMW，不改GPT，不允许父LUN/原分区写入。
- 真实标准 BlockIO 用专有Vendor device path，未伪造SimpleFileSystem或fs0。标准DiskIo/EnhancedFatDxe可在此4096-byte superfloppy上提供真实FAT12；格式化由root后续独立处理。
- profile不发布原父LUN BlockIO。Publish之前检查既有BlockIO与SFS均为0；存在其他卷或查询结果不完整/警告时拒绝，防止Shell拿到原文件系统。
- Open必须匹配固定durable PC whole-gap attestation，fresh capacity/FUA/WP/unitdesc/two flags与完整live主备GPT；仅Open最初扫描3584个完整原始零块。扫描后再次fresh gate，防止长扫描期间漂移。

PC attestation沿用固定capture1九份文件与test57校验输出 `PianoUfsWriteTestBaseline.h`。每个写与flush重新检查嵌入的固定size/SHA及已验证PC whole-gap记录。这是准备profile时电脑端独立验证的备份证明，不是固件在每次写之前连接电脑查询备份仍存在；FAT改变gap后也不能宣称有新的PC当前内容副本。原始whole-gap恢复基线保持不变。

## 每次写、flush和失败

`PianoUfsBoundedBlock.c` 每次WriteBlocks拆成单个4096-byte物理块；每块在回调前登记attempt、置Dirty/NeedsRecovery，并独立snapshot TX。

每块执行fresh五项capability/WP + 完整双GPT → WRITE10 FUA → 固定全窗SYNC10 → 清空/重新接收的独立READ10并逐字节比较。私有transport leaf也重复fresh gate，不能用恢复接口绕过能力/GPT校验。WRITE与SYNC布局逐字节核对只允许该gap单块或固定全窗SYNC，方向/OCS/GOOD/tag/LUN/residual/sense/data长度由共用Submit严格确认。FlushBlocks必须fresh gate、全窗SYNC和真实完整queue/IRQ quiet readback。

多块请求可部分成功；状态记录LastRequestBlocks、LastCompletedBlocks、累计VerifiedBlocks、WriteAttempts、最后logical/physical、attempted/returned及write/sync/read/quiet status。任何失败立即quarantine、Media.ReadOnly=TRUE，正常IO不再重试；Reset不会抹状态。最后Release也返回EFI_STATUS，若最终quiet失败不能对外返回成功。

所有**成功**写仍保持Dirty/NeedsRecovery。该卷只用于一次session；整段恢复和独立校验之前，timer/Halt、异常恢复和Cleanup均检查bounded状态，不能ResetSystem或free后进入下一启动。保持队列不明时Active DMA及TPL，只有完整doorbell/run/IRQ再次确认quiet才能retire，之后专用恢复可安全复用映射。每次IO与整个root恢复序列保持TPL_CALLBACK来阻止普通恢复timer穿插。

## Root 的恢复接口

`PianoUfsBoundedTransport.h`提供私有函数，不注册任意写协议：

```c
CONST PIANO_UFS_WINDOW_STATE *PianoUfsBoundedTransportState(VOID);
EFI_STATUS PianoUfsBoundedTransportCloseForRecovery(PIANO_UFS_WINDOW_IO *Recovery);
EFI_STATUS PianoUfsBoundedTransportVerifyRestored(VOID);
```

`PIANO_UFS_WINDOW_IO`包含既有 Read/WriteFua/Sync/ReadGuard/Quiesced callbacks，以及返回EFI_STATUS的Acquire/Release。Read仅允许gap与四个精确完整metadata blob：primary LBA1/4096、entries LBA2/12288、backup header LBA378879/4096、backup entries LBA378873/12288。WRITE只允许LUN4/window内单块4096；SYNC只允许LUN4/LBA375040/14680064。

推荐root序列：

1. 完成SFS操作并关闭文件/退出Shell。
2. 调用CloseForRecovery：先请求Disconnect，让FAT正常Stop可flush；随后关闭public protocol IO。
3. 即使FAT Stop失败，仍关闭public IO并允许专用恢复。`ConsumersDetached`/`DisconnectStatus`准确保留结果；不能将关闭public IO冒称消费者已断开。正常Stop仍须真实Disconnect成功后才能释放。
4. `Recovery.Acquire(Recovery.Io.Context)`，全程保持，使用root自己的whole-gap durable restore ledger逐块恢复原始PC备份；每步必须检查FUA/count/quiet，最终全窗SYNC。
5. **仍持有同一Acquire时**调用VerifyRestored；它支持同recovery context嵌套，不提前降TPL。必须fresh全guard/双GPT、独立完整3584块与原始零gap逐字节相等，并在长读之后再次fresh gate，才清Dirty/NeedsRecovery/Quarantined、置RestoreVerified。
6. 调用 `Recovery.Release(Context, EFI_ERROR(Status))` 并检查其返回状态。不得在restore和最终证明之间先Release，避免到期timer在仍Dirty时fence。
7. 检查状态无Dirty/NeedsRecovery/quarantine，再进行Halt/正常Stop/受控冷重启与Android外部复核。VerifyRestored不重新开放已关闭的卷，不允许FAT把缓存写回已恢复gap。

这里**未实现或自动触发恢复写**。Root必须集成完整恢复ledger和session结束顺序之后才启用物理FAT试验。没有BOOL acknowledgement能伪造restore证明。物理断电/外部强制复位不受软件TPL fence控制，FAT也无多块原子事务；外部原始whole-gap备份仍是恢复依据。

## 构建接入清单与验证

下一独立profile需复制这些新文件到RamApp source目录：PianoUfsBoundedBlock.c/.h、PianoUfsBoundedLayout.c/.h、PianoUfsBoundedTransport.h、PianoUfsBoundedBindings.inc。INF Sources增加两个.c；继续链接PianoUfsWriteTest.c/PianoGpt.c及BaseCryptLib、BaseMemoryLib、DebugLib、MemoryAllocationLib。INF Protocols必须含gEfiBlockIoProtocolGuid、gEfiDevicePathProtocolGuid、**gEfiSimpleFileSystemProtocolGuid**，Guids含gEfiEventExitBootServicesGuid。使用已验证PC header；Fat/EnglishDxe/Shell的DSC/FDF由root专用profile另接，不能同时发布原父卷，也不能走普通SimpleInit/OS自动启动。

`bash tools/test_ufs_bounded_block.sh`：15个actual C fixture场景覆盖默认关闭、外部基线拒绝、Open最后gap块非零、只扫描一次、64-bit/末边界/size/MediaId/buffer alias、unaligned客户、多块partial success、FUA/flush/RX/quiet失败、quarantine拒绝重试、Close后无法写、非零gap不能清Dirty、真实wholegap读取证明才能清除、最后Release失败与busy/reentry。固定builder还对照packed UFS ABI并拒绝越窗/非LUN4/错误size。

同脚本运行actual bounded Submit：golden gap布局、scope拒绝、全窗SYNC、未知queue保留且不reset、confirmed quiet退休、嵌套恢复/TPL、Dirty timer/exception fence、既有BlockIO/SFS和不完整inventory拒绝；ASan/UBSan通过，bounded宏下三个production translation units AArch64语法通过。原固定事务82例、原transport16+9例、原FAT/Shell/lifetime tests也通过。全部controller/DMA/介质写回调都是主机内存模拟；fixture header只临时render后删除，不运行prepare/main，不改staging。

Reserved DMA/table仅对消费EFI memory map的OS有保障；raw handoff仍需DTB显式保留range。这个session后端不证明raw/Windows/完整OS handoff，也不能替代下一轮硬件FAT与外部whole-gap验收。
