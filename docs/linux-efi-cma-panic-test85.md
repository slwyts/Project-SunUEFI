# test85：固定CMA与EFI sparse memmap契约故障

本次只读审查输入是`private/analysis/ramlog-test-85/console.txt`、同机captured live.dtb，以及memory-debug commit `c4bbf928f335174f8518831797a94597a530c575`的实际Image/vmlinux与源码。CMA、OF reserved-memory、memblock、sparse代码相对官方7704没有diff。没有修改kernel、DTB、platform memory map、CMA配置、pins或设备。

**真实panic已经定位到一个没有struct-page映射的固定CMA PFN；不能再只描述为容量不足猜测。** mainline已完成EFI/setup_arch/memblock/paging/bootmem/MM/scheduler/console与8CPU启动；在PID1的kernel_init执行core_initcall CMA activation时，`pfn_valid(0xf3800)`触发WARN，但仍继续把该PFN转换成struct page并解引用，产生L2 translation fault和Attempted to kill init panic。PID1这里是kernel-init线程，用户RAM `/init`还没执行。

## 地址与本机证据

| 项 | 证据 |
| --- | --- |
| DT节点 | `/reserved-memory/trust_ui_vm_region@f3800000` |
| 属性 | shared-dma-pool、reusable、无no-map、reg=`0xF3800000/0x05000000`、4MiB alignment、phandle0x71 |
| 范围 | `[F3800000,F8800000)`，80MiB；captured DT memory确实覆盖`[E3720000,F8800000)` |
| EFI / Linux memory | 最后实际map以及early memory node ranges均没有`[D8000000,FC800000)`，F380位于此洞内 |
| 创建CMA | console6375–6377：created CMA at F3800000 size80MiB、initialized trust_ui node、map reusable |
| invalid PFN | console6960附近：mm/cma.c168 WARN，x25=`f3800`，count0x5000pages |
| fault | VA`fffffdffc1ce0000`，ESR96000006、FSC06 L2 translation fault、PC init_cma_reserved_pageblock+20 |
| panic | 最后Kernel panic Attempted to kill init exitcode0xb；8CPU之前已全部started at EL1 |

`PIANO_PAGING_AFTER`4848、`UNFLATTEN_AFTER`5172、`BOOTMEM_AFTER`5322、`SETUP_ARCH_DONE`5697、`MM_CORE_AFTER`6354、`CONSOLE_AFTER`6787均存在。最后`PIANO_PRINTK_PANIC_BEGIN`后面的panic属于本次主线，不能与文件前面的旧Android记录混读。Android已由root观察恢复，26boot hash一致；回收console773752bytes/SHA `6c6c5de09aafb185c6fbd4c491b6e745a5559c263a64cd63e34e60fd20fcf83c`，没有新用户态成功证明。

## 不是memblock_add重新添回RAM

链条按源码实际顺序为：

1. `drivers/firmware/efi/efi-init.c:186`非KHO路径丢弃DT memory，再按cached EFI descriptors重建memblock.memory，并将不usable的类型标nomap。最后map不包含F380所在物理区域。
2. `arch/arm64/mm/init.c:304`扫描DT reserved-memory。固定reg、无no-map走`drivers/of/of_reserved_mem.c:136`的memblock_reserve。它只加入memblock.reserved；`mm/memblock.c:1007`的__memblock_reserve调用add_range(reserved)，不检验或扩展memblock.memory。
3. `unflatten_device_tree`中的`fdt_scan_reserved_mem_late`初始化固定reg nodes。`kernel/dma/contiguous.c:545`的rmem_cma_setup验证reusable/无no-map/对齐，再调用cma_init_reserved_mem。
4. `mm/cma.c:273`只验证非零size、memblock_is_region_reserved和对齐。并未要求整个range属于memblock.memory或都有有效PFN。因此洞内固定reserve也可登记CMA。cma_new_area还立即把count加到totalcma_pages；登记数不证明可用页或vmemmap已存在。
5. sparse_init依据memblock memory的present sections填充vmemmap。4KiB config的SECTION_SIZE_BITS=27，F380所在`[F0000000,F8000000)`128MiB section没有EFI memory，因此没有对应struct page PMD。
6. `mm/cma.c:168`仅WARN_ON_ONCE(!pfn_valid(base))，未return；下一循环无条件调用`init_cma_reserved_pageblock(pfn_to_page(pfn))`。实际vmlinux反汇编证明+20是首个struct-page flags load；每page步进0x40，即sizeof(struct page)=64。F380指针落在未映射vmemmap PMD，和日志L2 fault吻合。

因此“重添”发生在reservation/CMA registry与计数意义上，**没有重新成为System RAM或创建vmemmap**。`memmap_init_reserved_pages`使用for_each_valid_pfn跳过invalidPFN，所以早期reservations初始化没有在同一点fault；后来的CMA activation没有同样过滤。

CMA activation是core_initcall，normal ramoops_init是postcore_initcall，本次在前一个level就panic，尚未进入ramoops probe。这解释为何early RAM dumper能收回真实panic、normal dmesg/pmsg文件却没有生成；不能据此称ramoops初始化自身失败。

## 803008K与421888K账本

独立解析test85最后49descriptors，`is_memory`的WB/WT/WC总和正好**803008KiB**，与Memory行分母完全相等；严格usable type且WB为392916KiB。这证实分母包含EFI reserved/runtime cached-memory，不是被OF加回的整份15GiB DT RAM。

唯一CMA池按地址/size去除checkpoint快照重复后合计**412MiB=421888KiB**：固定qdss32MiB+trust_ui80MiB，动态成功300MiB。动态non_secure_display252MiB、cnss_wlan32MiB和>4GiB dump90MiB均有真实allocation-failed日志。Memory行available11684KiB确实显示严重内存压力，但该次首先证明的fault是F380缺struct page，不是分配返回ENOMEM。

另一个必须列入完整契约审查的固定池是`/reserved-memory/qdss_apps_region@82800000`，reg82800000/02000000、reusable共享池。它也不在当前EFI memory，但所属`[80000000,88000000)`128MiB section因8196/824A附近cached区域而有vmemmap。generic pfn_valid对early section可返回true覆盖section内的hole，所以没有在它的起点触发本次同样fault；这不证明该池已得到完整System RAM/linear-map/所有权契约。不能只检查单个pfn_valid或只修F380点。

## 给fullmap / 1GiB pool审查的约束

root与USB agent的1GiB fastbootboot pool工作应与Linux最终memory契约分开验证。物理高DRAM存在、UEFI MMU可访问、EFI allocator可用、Linux sparse/linear map可用是不同证据；cached reserved descriptor也不能直接等同available RAM。新候选pool必须覆盖真实同机memory并排除固定secure/firmware/CMA carve-outs、完整DT reserve与live image/stack/heap。

对每个enabled reusable shared-dma-pool，核整个PFN区间与最终EFI→memblock.memory、nomap、linear mapping、sparse present sections、zone和reservation的关系。不能把trust_ui名字对应区域直接标free conventional，也不能以“补一个vmemmap”证明所有权或physical-access安全。只提供更多高DRAM capacity而仍遗漏低地址固定CMA契约，也不自动解决F380 fault。

这份审查没有实施关闭CMA、删除DT节点、伪造provider/fullDRAM或将WARN改成产品修复。下一步需要root建立正确board/native EFI memory ownership，或进行明确单变量的诊断对照，再用本机markers/panic文本/新完整map检验。当前memory-debug Image和旧094d策略保留，Pogo阶段1也已冻结。
