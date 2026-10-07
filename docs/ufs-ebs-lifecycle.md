# UFS / owned SMMU 的 EBS 生命周期收敛

2026-10-05：本次完成源码和主机检查，没有 prepare、完整固件构建或设备操作。不是完整 Linux/Windows OS 交接证明。

## 停机与正常回收

`Quiesce()` 先确认当前仍为 legacy queue 模式；MCQ 返回 Unsupported，不能用 legacy 寄存器假装停止 MCQ。两个 run 寄存器 UTRLRSR/UTMRLRSR 都置零，读取完整 32-bit transfer/task doorbell，再使用 write-zero-to-clear 的反掩码取消全部当前 pending 槽。每轮都读取 transfer doorbell、task doorbell、transfer run、task run，四个 DWORD 全为零才成功。

循环最多 100000 次，间隔只执行 `CpuPause()`，不调用 `gBS->Stall`、timer event 或 native HAL。这是固定迭代上限，**不是毫秒计时或实际硬件停机耗时的证明**。Halt 还关闭/读回 UFS IRQ enable，日志保存最终四个 queue 值和 IRQ 值；没有重置 HCE、PHY 或其他 SMMU stream。

正常 Stop 仍先 DisconnectController 消费者，再 halt/卸载协议。Cleanup 在两个引擎都停止时恢复并读回原 transfer base/upper/IRQ，才解除 DMA 映射与回收数据、队列和 owned SMMU 表。owned Close 的原有 detach 读回及“其他 stream 不变”检查保留。安全回收完成后才恢复原 UTRL/TMR run，再读回验证；没有在表/映射释放期间先重启继承的队列。

停止或基址恢复失败时不进入后续 free。失败走 cold reset；ResetSystem 意外返回时进入 CpuDeadLoop，不能继续完成 EBS 或释放可能仍被访问的内存。

## Reserved 内存与 EBS fence

Shared DMA device 增加 `ReserveAcrossExit`，buffer 记录实际请求的 EFI MemoryType。普通 device 默认为 BootServicesData；owned UFS device 的队列/data，以及所有 owned SMMU table allocation，**从最初 AllocatePages 就使用 EfiReservedMemoryType**。正常 Stop 可以用 FreePages 回收尚未进入 EBS 保留状态的 Reserved allocation。

EBS hook 先 halt，随后对三份 UFS DMA buffer 和 SMMU table 调用 allocation-free retain fence。必须是已有 Reserved allocation、idle、非 quarantine 状态；错误内存类型或不确定状态拒绝并 cold reset。不会在最终 MapKey 之后改类型/分配页，不调用 native HAL、detach/free、关闭 event 或卸载协议。资源标为 ExitRetained 后，DMA Unmap/Free、新映射/提交、owned Close 都拒绝回收或修改该状态。

保留的硬件 SMMU 表和被其映射的 UFS DMA 页因此不会作为普通 Boot Services 页交给 **正确消费 UEFI memory map 的 OS** 回收。这不是 raw handoff 的自动保障：当前 raw Linux profile 没有该 UFS DMA provider，使用 stock FDT；未来任何包含这些 UFS 资源的 raw/手动跳转必须显式把完整 retained allocation ranges 加到 DTB memreserve/reserved-memory，并单独验证。仅有 ReservedMemoryType 不能防止忽略 EFI map 的内核按 stock FDT memory node 重用这些页。

EFI-map 消费、kernel 接管旧 SMMU stream、OS 自己的 host reset、其他 DMA master、运行时服务、Windows ACPI/设备支持仍未通过这轮实机验证。该改动不会把 raw Linux 或 Windows 描述为已完成安全交接。

## quiet active-poll 同步接口

`PianoDmaSyncForCpuQuiet(Buffer)` 与普通 SyncForCpu 使用相同的 active/mapped/direction/quarantine/exit 检查。**每次仍执行真实 InvalidateDataCacheRange + MemoryFence**，不会以减日志为由跳过缓存同步或把 DMA 标成完成；只增加 `QuietSyncs`，不打印每毫秒的双份日志。

`PianoDmaReportQuietSync(Buffer)` 可在收到事件或停止前调用，输出带 CRC mirror 的累计 `polls`、自上次已输出记录的 `delta` 和当前 active 状态。无新 count 不重复打印；调试输出关闭时不丢掉尚未报告的 count。普通 SyncForCpu 的原有 `sync-cpu` Log 格式及 begin/complete 提交日志保持原样。

Root 可将 USB ring 的 active polling 调用替换为 Quiet，并在 event/退出处显式 report：

```c
Status = PianoDmaSyncForCpuQuiet (&mRing);
// Read/process a device-owned event only after successful sync.
PianoDmaReportQuietSync (&mRing); // at event receipt and before final Free
```

本次没有修改 USB source，也没有宣称 quiet API 已在 USB 实机集成。

## 主机检查

- `bash tests/native/test_dma_foundation.sh`：ASan/UBSan 验证实际 Reserved type、普通 Stop 回收、EBS 后回收拒绝、owned table retain 无 native HAL、原有 DMA/cache/rollback 契约、quiet 每次 invalidate/不逐次 log/双份汇总及默认日志保持。
- `bash tests/native/test_ufs_firmware.sh`：高位 transfer/task 槽、两个 stuck run、stuck doorbell、MCQ 拒绝、固定轮询上限、bases/IRQ 在回收前验证、run 在回收后恢复、`gBS=NULL` 时 EBS 路径无 BS 调用/free、错误 memory type 拒绝、失败保留与 reset 返回后的 fail-stop。
- Dma/OwnedSmmu/UFS 源码 AARCH64 语法检查和 `git diff --check`。

[UEFI Boot Services 规范](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html) 定义 EBS 回调和 memory map 生命周期；[Linux UFS HCI register 定义](https://github.com/torvalds/linux/blob/master/include/ufs/ufshci.h) 与固定源码的 list-clear/run 寄存器一致。源码收敛和主机模型检查不能替代真实硬件读回与 OS 接管测试。
