# EFI test88：CMA激活以后常规页耗尽

实际test88加入两个occupied CMA descriptors后，test85的`init_cma_reserved_pageblock+20`/F380 PFN缺失L2 translation fault已不再出现。内核完成8CPU/MM/console并进入后续subsys initcall，新的panic为`System is deadlocked on memory`。本结论来自已回收console及固定内核源码；Root报告自动恢复Android、boot SHA26一致，本轮没有设备操作。尚未进入用户PID1 `/init`，不能标作RAM smoke/daily验收。

证据文件`private/analysis/ramlog-test-88/console.txt` SHA `cfea56f5677aae593c099bf9c0ba90f5c0ee17be6ea73e1eff97bb400962ff16`。该文件前段有Android日志与warning，后段有重复early printk snapshots；下面只核对`PIANO_EFI_ENTER_KERNEL`以后的mainline阶段与最终`PIANO_PRINTK_PANIC_BEGIN`，不将Android warning归于本次mainline，也不把重复snapshot重复计入统计。内核topic commit仍`c4bbf928f335174f8518831797a94597a530c575`，upstream base7704c4c5bb127673b4f0ead839919db573559e38；无源码/config/pin变化。

## 两个新EFI占用区的实际观测

EBS_ENTER为52 descriptors、2496bytes、stride48、version1、key363；EBS_RETURN status0/success1。

| 归档map | EFI范围，half-open | 类型/属性 | 对应DT |
| --- | --- | --- | --- |
| index2 | 82800000..84800000，pages2000 | type2 LoaderData，attr100F | qdss_apps_region：32MiB，map/reusable/shared-dma-pool |
| index43 | F3800000..F8800000，pages5000 | type2 LoaderData，attr100F | trust_ui_vm_region：80MiB，map/reusable/shared-dma-pool |

这证明两个完整type2 EFI descriptors已在实际EBS snapshot出现。`attr100F`含WB cache capability，符合Linux EFI可用RAM判定；它同时包含其他cache capability位，不能据此断言CPU实际cache类型。两candidate源row是`ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP`，真实Mu helper/attribute converter的host执行也验证WB/XN转换，但本次没有实际page-table walk/AT/XP读回。不得把`100F`当作EFI_XP位或XP硬件验收。

52条map的Linux `efi_is_memory`范围合计939720704bytes，即917696KiB，和最终`Memory: 9396K/917696K available`完全一致。usable WB合计519786496bytes，和is_memory不同；Reserved WB仍会被EFI rebuild标nomap，不能混入普通可用RAM。比test85的803008KiB分母增加114688KiB，正好为两个fixed CMA池共112MiB。

mainline日志6001..6003/6027..6029再次确认fixed QDSS和TrustUI pool创建/范围。pool创建本身不证明CMA activation；本次进一步证据是core_initcall以后已进入`ptp_classifier_init`/`sock_init`，最终412MiB全部成为free CMA buddy blocks，没有mainline CMA activation error/WARN或旧paging fault。结合`mm/cma.c:168..215`，这是本次activation完成的可靠阶段推断，没有直接读取`CMA_ACTIVATED`bit。

## 新OOM机制

最终分配链为`ptp_classifier_init → bpf_prog_create → bpf_int_jit_compile → bpf_jit_binary_pack_alloc → execmem_alloc → vmalloc → alloc_pages → out_of_memory`。日志GFP为`0x2cc2(GFP_KERNEL|__GFP_HIGHMEM|__GFP_NOWARN)`，order0；没有`__GFP_MOVABLE`。

| 最终内存数据 | 数值 |
| --- | ---: |
| total RAM | 917696KiB / 229424pages |
| reserved | 481212KiB / 120303pages |
| managed | 436484KiB |
| CMA registered/reserved | 421888KiB / 105472pages / 412MiB |
| managed减CMA | 14596KiB |
| free / free_cma | 105472 / 105472pages |
| buddy块 | 103 × 4096KiB，全标(C) |
| PCP残余 | 18pages / 72KiB，不能视作全局可满足分配保证 |

`mm/page_alloc.c:3786`的`alloc_flags_cma()`只给`MIGRATE_MOVABLE`分配`ALLOC_CMA`资格；3585附近watermark计算对没有资格的分配主动减掉`NR_FREE_CMA_PAGES`。fallback也不能将CMA转给任意unmovable分配。因此总free虽有412MiB，此次JIT/GFP_KERNEL可使用的普通buddy页已耗尽；这不是高order碎片化，也不是CMA激活再次故障。

OOM dump没有匿名/file/cache页、尚无用户任务；`mm/oom_kill.c:1155..1166`在找不到killable victim时为普通系统OOM调用panic。日志7818/7819为明确`Out of memory and no killable processes`和随后deadlocked panic。PID1此时仍是kernel-init thread，不能用“杀掉init后自动reboot”解释本次路径。

这里BPF/PTP只是当前第一个暴露普通内存耗尽的分配点。禁用该驱动或CMA不能作为完整DRAM/EFI契约修复。本轮没有改内核、删除CMA或增大真实公告容量。

## 后续离线prototype范围

[高RAM occupied prototype审核](piano-high-ram-occupied-prototype.md)为A00000000..A40000000生成一个默认关闭的独立45-row表，保留88的44行，目标是EBS后提供普通System RAM。它目前不在真实target内；ownership、动态DT保留区UEFI放置、实际AT/page-table/cache/permissions和DXE reservation验收全部待Root审查。也不等于preboot1GiB下载池已经可分配。
