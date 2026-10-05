# Piano全DRAM合同：离线review，尚未应用

`tools/plan_piano_dram.py`将固定captured live.dtb、当前native C descriptors、参考native JSON、归档test85 EFI map与可选Android runtime证据组合成可复核JSON。工具不输出MemoryMapLib C表、不调用MMU/allocator/hardware、不改kernel/pins/prepare/shared。所有candidate的hardware/MMU/allocator ownership均未验证，不能直接部署。

复现：

```sh
python3 tools/plan_piano_dram.py \
  --runtime-evidence private/analysis/dram-runtime-android-after-test-86.txt \
  --output artifacts/dram/piano-full-dram-review.json
python3 -m unittest discover -s tests -p test_plan_piano_dram.py -v
```

DTB SHA固定a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7；其他输入按实际bytes/SHA记录，发布前重核。不同DTB不能通过CLI替换。输出源是review snapshot，root后续更新native或runtime证据必须重新生成并审查；不能把旧JSON当现行表。

## 当前输出与保留原则

17个memory tuples中1个zero placeholder，16个非零合计16470685696bytes=15.339521408GiB；保留3个zero记录（memory D8600000及两个demura），不将它们变成区间。47个非零fixed reserved tuples及14项dynamic constraints全部保留来源/enable状态。动态12个reusable CMA请求、debug_kinfo4KiB和dump_mem90MiB是约束，不是固定占用。

当前72段page-aligned DRAM review segments完整、不重叠地覆盖DRAM page envelopes；所有native rows原样保留，`new_descriptor_candidates`只含没有native provenance的新增建议。不得把包含preserve_native的分析segments再与native rows拼接成表，那会重复映射。现有FD/heap/kernel/window及其UC/WT/NS_DEVICE/WB/XN属性优先，工具不会重写。

native C与参考JSON有4项差异：C新增FD_Reserved A7000000..A7100000与BootHandoff末4KiB，UEFI_FD改为A7100000..A7400000，UEFI_RESV截至A7FFF000。planner以当前C解析值作保护事实；JSON只比较，不能把它当实际compiled table。

DT dump_display81CF4000..81CF4800占2KiB，普通DRAM从同页81CF4800开始。该整页81CF4000..81CF5000标protect_partial_page，禁止分配；raw memory bytes与page envelope bytes分别记录，padding没有伪装成可用DRAM。固定reservations向外round页用于保护，普通candidate不吃跨页未知字节。

现存4个declared-conventional/archived-runtime与DT reservation交集单列conflict：DBI_Dump与adspslpi约87MiB、与xbl_ramdump256KiB（NoHob与UC，实际allocator语义unknown）；DXE_Heap首320KiB与adspslpi；DXEheap中hwfence2932KiB，且2868KiB在85实际runtimecode中。这些不能通过新增高RAM、改变cache或直接覆盖descriptor默默消除。

## CMA的两阶段含义

固定qdss82800000..84800000（32MiB）和TrustUI F3800000..F8800000（80MiB）在85 EFI usable coverage均为0，是已证明的合同缺口。review建议其新增部分为EfiLoaderData/WB语义：**preboot不得新分配，EBS后是System RAM，随后仍由DT reservation/CMA管理**。两段的candidate post-EBS memory coverage完整，且均排除新free候选。它们的ownership/HOB/allocator/MMU验证仍false；枚举名不能替代真实保留动作。

默认EfiReservedMemoryType+WB虽满足Linux `is_memory`，但不满足`is_usable_memory`，EFI rebuild会mark_nomap。它不能自动解决reusable CMA需要的System RAM/完整PFN/linear-map契约。EfiLoaderData或BootServicesData+WB可在EBS后成为Linux usable RAM，但必须证明native HOB及allocator确实在EBS前保留该region，不能只把type写在纸上。Permanent firmware/no-map与unknown cache区域保留Reserved/unknown建议，不能猜WB或宣称可reclaim。

新free候选只从真实DT memory减去所有fixed reservations、所有native区域与page fringe。Disabled DT reserved仍按ownership未知保守排除，不因status disabled就释放；native PIL UC等始终保护。Runtime EFI allocations作为归档phase记录，实际ARM cache不可从attr100F能力位推断。

归档EFI里落在非native DTmemory的Loader/BootServices/Runtime/Reserved allocation也不能被新增free吞掉；这类标archived_preboot_ownership_protected。native有属性但归档EFI没有完整覆盖时，observed post-EBS/nomap字段为null unknown，不能把absence当确定否。

## High DRAM候选与动态phase

| DT-backed high候选 | 大小 | 证据边界 |
| --- | ---: | --- |
| 880000000..8AF6FD000 | 795856896bytes | 小于1GiB；Android86 dump90MiB在其中 |
| 8B0000000..8B5500000 | 89128960bytes | DT范围，无UEFI高map/MMU证明 |
| 8B5700000..8B7500000 | 31457280bytes | DT范围，无UEFI高map/MMU证明 |
| 8C0000000..C00000000 | 13958643712bytes（13GiB） | ≥1GiB候选；末4KiB有Android debug_kinfo观察 |

这些已共享给USB agent，仍不是可访问或可分配的pool。root需独立证明SoC物理所有权、实际ARM MMU映射、EFI allocation、完整DMA/secure carve-outs和启动产物lifetimes。

Android86的debug_kinfo BFFFFF000..C00000000（4KiB nomap）与dump_mem880000000..885A00000（90MiB map/nonreusable）只记录`android_runtime_observation`，`uefi_ownership_proven=false`。planner按这些endpoints细分注释，不将其视为当前UEFI allocation，也不把alloc-ranges整段视为占用。低4GiB envelope实际长度FFFFFFFF，并非100000000；动态分配需满足size/alignment/范围/未来其他消费者，当前plan不替kernel选择落点。

## Invariants与剩余验收

11项host tests覆盖Reserved WB与LoaderData WB语义区别、CMA不作preboot free、整段post-EBS coverage、部分页保护、dynamic envelope不变固定reserve、Android phase隔离、overflow/overlap/trace metadata拒绝、native语法未支持行不得静默跳过、非native归档EFI占用不得吞并、DTB pin漂移不发布以及真实同机全coverage/native差异。工具断言page-aligned/nonoverlap/完整envelope、native cache override0、新free不与native/fixed/归档active占用相交，仍明确输出unknown与4个conflicts。

这套invariants只证明离线合同和输入一致性。真正fullmap/HOB/缓存/secure/DMA边界、1GiB buffer可访问性、EBS后kernel sparse/CMA成功与Android恢复仍需root逐项实机验证。test85真实fault链见[独立审查](linux-efi-cma-panic-test85.md)；Pogo阶段1及memory-debug Ready包保留。
