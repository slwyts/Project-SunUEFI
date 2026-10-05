# 固定单块 UFS 事务 transport（第80次实机写入与恢复通过）

本阶段接通固定 LUN4、LBA375040、4096 bytes 的 WRITE10(FUA) / SYNC10 与现有 owned SMMU/shared DMA/Submit。现有 BlockIO 的 Media.ReadOnly 与 WriteBlocks 无条件拒写保持不变。两个测试 profile 都默认关闭；没有通用写块协议、任意目标 write builder 或持久变量修改。

## Profile 与外部基线

`tools/prepare_gui_profile.py` 增加互斥选项：

| 选项 | 实际行为 |
| --- | --- |
| `--ufs-write-preflight` | 主机备份校验 + 实机全部 live gate/read；绝不提交 WRITE 或 SYNC；不加载 SimpleInit/OS |
| `--ufs-write-restore-test` | 相同完整 gate 后执行一次 test WRITE(FUA) → SYNC → 独立 READ → restore WRITE(FUA) → SYNC → 独立 READ/hash；不加载 SimpleInit/OS |

两者隐含 `--ufs-blockio` 及原有 UFS/DMA/owned/foundation/keys/recovery 依赖，要求 `prepare_native_probe.py` 的已选组为 `ufs`；拒绝并用 FAT/Shell/Setup/USB/touch/QUP/fault-test 等额外消费者。第79次已运行只读preflight，第80次才运行restore-test；生成、构建与实际发送的镜像分别归档。

新增 `tools/prepare_ufs_write_test.py`：只读取电脑端固定 `private/captures/ufs-test-area-1` 和 test57 证据，核对九份文件独立的固定 size/SHA256、manifest 类型/几何、完整主备 GPT CRC/array/location、79 个活动且互不重叠的分区及 gap、14 MiB 全零、原块与相邻块、test57 MBR/header/entries。它再次独立打开全部文件和 manifest 检查读取期间漂移。输出原子写入/fsync/独立 readback 的 `PianoUfsWriteTestBaseline.h`，嵌入主备 header/array、原块与 gap SHA。profile 准备在 staging 变化前先验证 archive，并在生成 header 时再次完整验证。

Header 的 `ExternalArchiveVerified=TRUE` 只陈述电脑端备份已核对，不能替代 live gate。请求 `Authorized` 是显式 restore-test profile 的内部布尔 gate；preflight 可保持 FALSE，不会授予后续 Run 权限。

## Live gate 和受限提交

`PianoUfsWriteTestPreflight` 与正式 Run 共同执行：

1. 固定 tuple、容量1551892480 bytes、外部 baseline 的 pinned SHA/size/hash。
2. fresh LUN4 READ CAPACITY16（4096 bytes/block 与完整32 bytes response）、MODE SENSE10 当前 caching page（FUA1、mode WP0）、unit descriptor（ID/index一致，unit WP0/1/2），再分别读取 permanent WP 与 power-on WP flag。所有五项都须成功、采集 mask0x1F；两个启用 flag 都须0。UnitWP1/2 是配置类型，只有对应 flag 明确未启用时才能通过，未知/保留值拒绝。
3. fresh 完整 primary header/array 与 backup header/array，逐字节匹配电脑端 baseline，并重新验证 CRC/对偶位置/分区边界和 gap。
4. 3584 个完整 READ10 扫描 LBA375040–378623 的 14 MiB gap，全零；两次独立目标块 READ 匹配原块与 SHA。
5. 扫描之后再次 fresh 全五项 capability/WP 与全部主备 GPT；preflight 到此返回，不调用 WRITE/SYNC。正式 Run 才进入首次 WRITE。
6. 正式 Run 在 restore WRITE 前再次 fresh 全五项 capability/WP 与全部主备 GPT。若发生漂移，停止恢复写入并保留 recovery 状态。

`Submit` 默认拒绝 WRITE/SYNC、未知 CDB、write query 与未知 UPIU。preflight 即使误调用写回调也拒绝。restore-test 必须在唯一执行 fence 内、shared mData 为 Bidirectional，并且32-byte UTRD和1024-byte UCD逐字节等于固定 builder 的结果；任意不同 target、size、FUA、tag、PRDT 或 CDB 都在 doorbell 前拒绝。READ 方向日志为 from-device，WRITE 为 to-device。WRITE/SYNC要求 OCS0、GOOD、正确tag/LUN、零 sense/residual/overflow/underflow及精确传输长度。

复用 mTrl/mUcd/mData；只该 profile 的 mData 改为 Bidirectional。每次 RX 将 shared bounce 清零，重新提交 READ10，通过 shared DMA complete/invalidate 后才复制到 helper 的独立 RX。TestPattern/Original/InitialA/InitialB/TestRead/RestoreRead 与 metadata buffers 彼此分离，不能把先前 TX 或未刷新的 RX 当作读回证明。

## 生命周期与最终证据

helper 在进入每个 WRITE 回调之前登记 attempt，记录是否 returned、tuple、status、transferred。Work 的 Running/NeedsRecovery 防止重入和未恢复重试。固件 profile 另有一次性 entered/started fence，不能再次调用 experiment 清掉 DMA、workspace 或 ledger。

事务全程提高到 TPL_CALLBACK，从完整 gate 一直到 verified restore，阻止同为 TPL_CALLBACK 的恢复 timer 在 restore 前冷重启；所有其他 BlockIO读消费者在 busy 时返回 NOT_READY。Quiesced 回调使用有界直接 CpuPause 轮询，读取全部32-bit transfer/task doorbells、两 run regs 与 IRQ readback；固定 poll 上限是迭代次数，不能解释成毫秒。

第一次 Submit halt 未确认时，不盲目 ResetSystem 或继续复用 DMA。第二次完整 quiet readback 成功，才将 Active/quarantined buffers 真正 complete/invalidate并允许 helper 按失败恢复路径继续。持续无法确认 quiet、restore/gate/sync/readback 未验证，都会 retain DMA/page-table/ledger、拒绝 Cleanup/Stop/后续boot，并在仍持有 timer TPL 时 CpuDeadLoop。现有异常 diagnostic 也先打印 ledger，在 NeedsRecovery 时 fence 通常的 exception cold reset。物理断电或外部强制复位仍不受该软件 fence 控制。

最终证据保存在静态 RAM，并由 Halt 再次输出：

- `SUNUEFI_UFS_WRITE_REPORT_BEGIN`：mode、started/returned/executing、固定tuple、实际 doorbell提交次数（不等同物理介质已写证明）。
- `SUNUEFI_UFS_WRITE_GUARD`：最多三轮 fresh guard 各值和五项 status。
- `SUNUEFI_UFS_WRITE_RESULT`：outcome、gate/first failure/final、attempt次数、test/restore match、restored_verified/data_unchanged/safe_to_continue/requires_recovery/quarantined。
- `SUNUEFI_UFS_WRITE_STEP`：19个步骤的 attempted、calls、bytes、status及quiet状态。id0/1–4为最初guard/GPT；5 gap；6/7独立初始读；8/9/10 test write/sync/read；11/12–15 restore guard/GPT；16/17/18 restore write/sync/read。
- `SUNUEFI_UFS_WRITE_ATTEMPT`：两个完整attempt；`SUNUEFI_UFS_WRITE_HASH`：test/restore SHA256；`REPORT_END` 明确boot_blocked和OS handoff未验证。

Preflight通过的 outcome=5，必须 started1/returned1、write_doorbells0/sync_doorbells0/attempts0、gap calls3584/bytes14680064、guard calls2、主备各 metadata step calls2、initialA/B各1、WRITE/SYNC/restore steps未执行。其 data_unchanged/safe_to_continue1指完整只读事务通过；restored_verified仍0。

正式成功的 outcome=1，两个WRITE和两个SYNC doorbells、两个attemptreturned1、test_match1/restore_match1/restored_verified1/data_unchanged1/requires_recovery0/quarantined0，restore SHA必须为 `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7`。outcome=2表示测试阶段失败但restore已验证，必须保留 first_failure。outcome3/4不能宣称 data_unchanged或允许下一启动。所有受控profile在安全完成后也停留RAM，等待原timer触发Halt最终报告。

Reserved UFS DMA/table retention 只对消费 UEFI memory map 的OS有保障；raw Linux使用stock FDT且不自动消费EFI map。将来raw UFS交接仍须给所有保留range加入DTB memreserve/reserved-memory。第80次证明了该固定块的物理WRITE/FUA/sync/readback/restore闭环，未验证突然断电持久性、通用可写BlockIO、FAT写入、raw/Windows或完整OS handoff。

## 实机证据

第79次preflight返回outcome5，started/returned1，WRITE/SYNC doorbell和attempt全0；全gap扫描3584次、14680064 bytes，两轮fresh guard/GPT通过。自动返回Android后26启动分区SHA一致。

第80次真实运行返回outcome1，gate/first_failure/final均Success，WRITE门铃2次、SYNC门铃2次，两项attempt分别为test和restore，均完成4096 bytes。test_match、restore_match、restored_verified、data_unchanged、safe_to_continue均1；requires_recovery和quarantined均0。独立RX得到测试块SHA `7acc23a1439e0b8ab56db9ed454503090a9d34d365207f2dca13bac5f6416dee`；恢复SHA与原块 `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7` 一致。

归档镜像SHA `620520a1e9499adfe2baf17f741abe9a061e8664a005b0cb275a3782d6c0bea0`，build_id `6268eba4-ad3c-4f72-bfb0-a268c8f08334`。Android恢复后capture2重新读取全部九份基线对象：完整14MiB gap、原块、两相邻块、MBR、主备GPT头及数组，逐字节匹配capture1。gap全零，两次独立读取一致；26启动分区SHA一致、A槽位与root正常。此处没有修改GPT，空隙仍不构成永久专用分区。

可复核证据记录为 `artifacts/ufs/write-validation-test-80.json`。`tools/record_ufs_write_validation.py --test-id 80 --post-capture-id 2` 从现存归档重验固定PC pins、post-Android文件、最终事务报告、镜像SHA及26分区记录，再生成验证文件；该工具不连接或写入设备。输出以独占创建方式避免覆盖既有记录。

## 电脑端验证

可重现 host-only 验证入口：

```bash
bash tools/test_ufs_write_transaction.sh
bash tools/test_ufs_write_transport.sh
python3 -m unittest discover -s tests -p 'test_ufs_write_profile.py'
python3 -m unittest discover -s tests -p 'test_prepare_ufs_write_test.py'
bash tools/test_dma_foundation.sh
bash tools/test_ufs_firmware.sh
```

当前结果：helper82个actual C用例（含20完整preflight），真实Submit/guard/read/FUA/sync/quiet适配器16个restore-test + 9个preflight memory-only用例，packed ABI、ASan/UBSan、两macro profile AArch64语法检查通过；profile10项、attestation22项通过；既有DMA/owned/layout/BlockIO/lifetime/FAT/Shell host检查通过。

transport测试临时render已验证PC fixture到disposable include（不是运行prepare/main或产生固件staging），结束删除。覆盖doorbell前固定布局拒绝、默认/preflight拒写、独立RX、方向日志、摘要重发保持ledger、timeout已quiet恢复、首次halt不明后二次quiet成功退休、持久unknownqueue与restore失败停止、high-slot/TMR/IRQ读回。所有controller/DMA回调与介质都在电脑内存中模拟。
