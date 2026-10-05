# Linux 标准 EFI-stub handoff 审查

审查范围：只读 test73/74/75/78、当前平台内存表和官方 next `7704c4c5bb127673b4f0ead839919db573559e38` 源码。请求中的 `/home/slwyts/linux-piano/build/worktree-next` 本机不存在；实际审查目录是 `build/kernel-worktrees/next-7704c4c5bb12`，HEAD已核对。审查未改 LinuxRamBoot、prepare、memory map、设备或 stable/next refs。后续专用 debug kernel 在独立 topic/worktree 实施，见末节。

**test78 已确认 ExitBootServices 实际成功返回，标准 EFI 的可用 WB RAM 只有383.707MiB，远小于 raw DT 的15.34GiB；随后失败的精确阶段尚未确定。** 完整 map 已捕获，重定位 kernel 区间有强证据，final FDT 精确地址仍需直接 marker。OnExit event 本身仍不足以证明EBS返回；test78新增的返回 wrapper 才完成该区分。缺 kernel/pstore 日志不能证明 CPU 未进入 kernel。

## 对照证据

三次测试的 Image、initrd、显式 DTB hashes完全相同：Image `2a5aeaa3f97d78d0fc9222320fcf09ff54e6484cc49cf9c2b368b4ab9a77197f`，initrd `2197b2b2d41d53f3fafd161402a2f56fe178702e1279e872c2f5897a735af511`，DTB `a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`。它们对应同一 clean next build，区别是 raw ARM64 与 EFI-stub 交接。

| 测试 | 当前能证明的阶段 |
| --- | --- |
| raw73 | Linux `7.3.0-rc5-g7704...`；`efi: UEFI not found`；ramoops在0.080秒注册；0.674秒PID1 BEGIN；8CPU/RAM诊断；180.677秒主动重启 |
| EFI74 | payload hashes正确，FDT config table已装载，PE LoadImage、LoadFile2两阶段initrd交付；最后为native DBI/App Log Flush，无 next日志 |
| EFI75 | 同上，加 fault handler安装成功和 before/exit-boot-services两个事件；最后仍为native DBI/App Log Flush，无 fault marker/next日志 |

raw73 的日志明确显示约15.34GiB DT memory、`Memory: 14793636K/16084652K ...`，ramoops注册后能回收此前 printk。test74/75 回收的 console 中是恢复后的原机6.6 Android日志，`linux.txt` 为0bytes。这验证了恢复及捕获状态，未定位失败PC。存盘的 launch result仍是 pending observation，仅保存 fastboot约0.363秒发送/Booting时间；约20秒返回Android来自运行观察，不是这些JSON中的精确内核计时。

## 内存契约的实际差异

官方 `drivers/firmware/efi/efi-init.c:159` 的 `reserve_regions()` 在非KHO启动时执行 `memblock_remove(0, PHYS_ADDR_MAX)`，丢弃原先从DT memory获得的区域，再按EFI map重建。Loader/BootServices/Conventional等区域也必须有 `EFI_MEMORY_WB` 才可作为 usable System RAM。`efi=novamap`只避免SetVirtualAddressMap，不关闭这套EFI memory来源。

平台 `MemoryMapLib.c` 的唯一大块 Conventional 区是 `DXE_Heap [0xBD930000,0xD8000000)`，423MiB。其他若干BootServices区域可回收，但静态表没有 raw DT中数GiB的高地址DRAM banks。**423MiB仍可能启动最小系统，不能只凭容量就断言此次panic。** 必须读取实际最后GetMemoryMap的type/pages/attribute，而不能把HOB资源attributes或`WRITE_BACK_XN`直接解释成EFI descriptor的WB/XP位。

同机DT还揭示静态heap不是完全无保留的空洞：

| 与heap相交的区域 | DT保留性质 |
| --- | --- |
| `0xBD930000..0xBD980000`，320KiB | 落在ADSP/SLPI `no-map`保留区尾部 |
| `0xD4E23000..0xD5100000`，2932KiB | `hwfence-shmem`，`no-map` |

test74/75未记录最后relocation/FDT allocation，彼时无法排除allocator冲突。test78下一节的实际map已将重定位kernel和LoaderData候选区限定在无固定DT reserved冲突的位置。raw placement为A8000000 kernel、B0000000 DTB、B0200000 initrd，与标准分配路径不同。

libstub会删除FDT reserve-map entries并加入 `linux,uefi-system-table/mmap-*`；本机原FDT reserve map为空，因此“删除reserve map”本身不是已证实的丢保留问题。`/reserved-memory` nodes仍保留，并由`arm64_memblock_init()`处理。不能通过删no-map/secure carve-outs来扩大可用内存。

## PE及重定位

实际Image PE结构与`arch/arm64/kernel/efi-header.S`一致：

| 字段 | 值 |
| --- | --- |
| PE entry RVA | `0x1DE5388`，在text内 |
| SizeOfImage / ARM64 image_size | `0x26A0000`，40,501,248bytes |
| SectionAlignment / SizeOfHeaders | 64KiB / 64KiB |
| text | RVA/raw `0x10000`，长度`0x1E70000`，R/X |
| data | RVA/raw `0x1E80000`，virtual `0x820000`、raw `0x761A00`，含BSS尾部 |
| primary_entry RVA（System.map） | `0x1D3F0F0` |

UEFI记录初始ImageBase `0xD0F40000`、EntryPoint `0xD2D25388`，数学上与PE entry相符。它满足64KiB segment alignment，但不满足当前 `nokaslr`要求的2MiB alignment。`efi_get_kimg_min_align()`在nokaslr时返回`MIN_KIMG_ALIGN=SZ_2M`；`efi_kaslr_relocate_kernel()`因此不能直接执行这份初始PE，需要另分配并复制kernel。test74/75的LoadFile2和EBS事件说明流程已越过`handle_kernel_image()`，但未透露最终kernel address；test78的LoaderCode map提供了重定位区间的强证据，见后文。

重定位区使用`EFI_LOADER_CODE`，并尝试通过EFI memory-attribute protocol设置R/X与RW/XN。协议缺失/失败时`efi_remap_image()`可返回void继续；平台heap初始ARM映射为XN。因此还应记录最终LoaderCode pages的真实memory attributes和该协议状态。现有证据不能断言NX确实是本次故障。

## EBS事件和日志的边界

本机Mu `CoreExitBootServices()`顺序是 before-event →停timer→MapKey验证/TerminateMemoryMap→OnExit事件→MemoryProtection callback→禁debug timer/CPU interrupts→清BS/ConOut→返回。故test75的OnExit、native DBI/Flush在真正返回之前。当前MemoryProtection EBS callback主要放松runtime image保护，不是已证实把所有LoaderCode变成NX；具体执行结果仍需trace。

标准stub默认`efi_loglevel=LOGLEVEL_NOTICE`，其`efi_info()`信息被过滤；`loglevel=7`不等同`efi=debug`。缺“Booting Linux Kernel”或镜像OutputString marker不能作为stub未执行的证据。

raw73的ramoops直到约0.080秒才注册。若标准路径已进入`primary_entry/setup_arch`、随后在EFI map/memblock/paging阶段panic，早期printk未必已进入ramoops。`panic=15`加恢复boot时间与约20秒观察相容，但也可能是EBS尾部/早期异常或watchdog；现有时序不能区分。UEFI fault handler安装成功只覆盖仍使用该vector/映射的阶段，不能覆盖随后kernel换vector后的全部故障。

## 最小下一诊断

1. **先确认EBS真正返回**：已提供独立`PianoEfiHandoffTrace.c/.h`。Install包装GetMemoryMap/ExitBootServices，CRC重算，静态64KiB snapshot；进入EBS前dumpCPU与最后成功完整map，原EBS返回后只写RAM serial，不访问BS。成功后Restore拒绝；失败保留hooks供stub重试。输入容量、stride、version不可信时明确`known=0`，不打印伪完整map。
2. **开启`efi=debug`**：结合EBS返回marker观察是否可看到relocation/DTB阶段信息。不要先换Image/DTB或扩大内存，保证对照只增加trace。
3. **交叉审核最后map**：按EFI WB/usable type统计RAM，核对final LoaderCode/LoaderData allocations与所有同机`/reserved-memory`，尤其ADSP尾部/hwfence。新FDT address应从最后map和stub信息建立，不能沿用应用安装的旧FDT pointer。
4. **test78已证实EBS_RETURN success，仍无kernel日志**：独立debug topic给`efi_enter_kernel`、`primary_entry`、`setup_arch→efi_init→reserve_regions`设置简短阶段marker，使用已验证ramoops console，检查既有header后有限追加，不依赖pstore注册或BS调用。

trace host测试直接编译真实source，用独立zlib验证CRC，用PROT_NONE的BS table证明success后无读写；覆盖stale-key retry/error、overflow、stride/version、restore ownership。它不会allocate/free、lookup protocol或调用BS CRC。当前SerialPortLib先写RamLog再渲染framebuffer，集成时保持其无BS依赖，并考虑完整map增加的输出时间。root随后完成trace集成和AArch64编译，并执行test78；该实际返回与map证据见下一节。本agent没有执行设备试验。


## test78：完整最后 EFI map 的独立解析

本次输入为 `private/analysis/ramlog-test-78/uefi.txt`。行37记录 `key=0x364 known=1 bytes=2352 stride=48 version=1`，行91记录同次 `EBS_RETURN status=0 success=1 key=0x364`。49条 descriptor 与字节数一致，地址区间无重叠；不能继续把“EBS没有返回”作为当前最可能解释。CPU在EL1，`SCTLR=0x30D0198D`的M/C/I均为1，`TCR=0x480803514`。`linux.txt`依然为空，Android恢复后26项分区hash一致。

| EFI type | descriptors | pages（4KiB） | MiB |
| --- | ---: | ---: | ---: |
| Reserved | 11 | 194256 | 758.812500 |
| LoaderCode | 3 | 19921 | 77.816406 |
| LoaderData | 2 | 689 | 2.691406 |
| BootServicesCode | 7 | 1956 | 7.640625 |
| BootServicesData | 11 | 6727 | 26.277344 |
| RuntimeServicesCode | 6 | 13798 | 53.898438 |
| RuntimeServicesData | 3 | 309 | 1.207031 |
| Conventional | 5 | 68934 | 269.273438 |
| ACPI reclaim | 1 | 2 | 0.007813 |
| 全部 | 49 | 306592 | 1197.625000 |
| Linux usable type且WB | 29 | 98229 | 383.707031 |

usable 统计严格采用官方next `is_usable_memory()`，合计402345984bytes，尚未扣kernel/initrd/EFI map/DT动态保留/CMA。ACPI reclaim的8KiB会再保留。全部Conventional均落在DXE heap；该heap由33条连续descriptor完整覆盖，精确长度443351040bytes=422.8125MiB，其中usable367.7421875MiB，其余55.0703125MiB为runtime；heap外另有15.96484375MiB BootServicesData可回收。不存在完整高地址DRAM映射。

所有usable条目attribute均为`0x100F`，runtime条目为`0x800000000000100F`。该值包含WB以及UC/WC/WT/WP，没有XP/RO位。descriptor满足Linux的WB判定，仍不能直接证明实际ARM页表的执行权限，故不能仅据此排除XN。

| 对象 | 实际地址/区间 | 证据与限制 |
| --- | --- | --- |
| 原始LoadedImage | `[0xD0F30000,0xD35D0000)` | 行14/15；entry D2D15388 = base + PE entry RVA1DE5388 |
| 原始PE allocation | `[0xD0F2A000,0xD35DA000)` | map#12；0x26B0 pages，比SizeOfImage多64KiB；不能把allocation起点当ImageBase |
| 重定位kernel候选 | `[0xCE800000,0xD0EA0000)` | map#10 LoaderCode；2MiB aligned且长度恰SizeOfImage0x26A0000，强支持relocation |
| 应用安装的旧FDT | `[0xD35DA018,0xD36F9332)` | 行13，1176346bytes；在BootServicesData#13内，不等于stub final FDT |
| 合并LoaderData | `[0xCE550000,0xCE800000)` | map#9，0x2B0pages；能包含多个相邻allocation |
| 已确认initrd | `[0xCE750000,0xCE7F18C0)` | 行25/26，661696bytes，位于上述LoaderData内 |

CE550000到已知initrd起点恰2MiB，与一个2MiB FDT allocation相容，但只是布局推断，不能把整个LoaderData区域称为final FDT。若kernel relocation base为CE800000，则原Image的primary_entry RVA1D3F0F0对应D053F0F0；需要新stub marker直接记录真实目标和FDT指针。

captured live.dtb（SHA256 a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7，1110810bytes）的reserve-map为空；memory/reg有17tuples，其中1个size0，合计16470685696bytes=15.339521408GiB。上述kernel/PE/FDT候选/initrd都在有效DT memory内，均未与固定reserved-memory/reg相交。

EFI usable与有效固定DT no-map的实际交集是384KiB：ADSP尾部`[BD930000,BD980000)`320KiB属于Conventional#8；hwfence最后`[D50F0000,D5100000)`64KiB属于BootServicesCode#21。hwfence此前2868KiB已是RuntimeServicesCode#20，本就不可用。扣这两段后383.33203125MiB仍不是最终MemTotal。

更明显的容量约束来自原DT的12项enabled/reusable shared-dma-pool动态请求：16+64+56+8+36+4+12+252+16+16+72+32=584MiB，全部要求4GiB以下；另dump_mem_region90MiB只允许4GiB以上，而EFI map没有这些高地址DRAM。当前config启用CMA/DMA_CMA、CMA_AREAS20/default32MiB。动态分配可以失败并继续，所以不能据此宣布panic原因；却已证实当前EFI RAM契约无法兑现全部原DT池请求。marker应首先区分efi_init/reserve_regions完成情况，再决定是否追踪arm64_memblock_init动态分配。

## 独立 EFI entry debug topic（待 root 实机验收）

`/home/slwyts/linux-piano-efi-debug` 的 `topic/piano-efi-entry-debug` 从官方7704创建，clean commit为`094d0b053f61ca20f584e10faa624cd0bc745db0`。没有改stable/next refs、meta pins或板级内存表。新选项`CONFIG_PIANO_EFI_ENTRY_DEBUG`默认n，要求ARM64/QCOM/EFI/PSTORE_RAM/DEBUG_KERNEL/4KiB little-endian。硬编码A3500000只适用于此捕获板，不能在其他设备启用。

正式调试Image与config已保存到`artifacts/kernel-topics/piano-efi-entry-debug/`：

| 产物 | 大小 / SHA256 |
| --- | --- |
| Image | 39721472bytes，`7091941820b74407d5dd0575495afdea29e8f7ecde699705807cc817f0135ce9` |
| config | 314102bytes，`a7fae7617530e464c9c624bf4d5a16c760060c45b0db8b82a44d9b26c02fc14c` |
| manifest.json | commit、compiler、config delta、PE字段、各产物hash和pending hardware gate |

相对已验next config只改变debug gate=y、LOCALVERSION=`-piano-efi-entry-debug`和LOCALVERSION_AUTO=n。release为`7.3.0-rc5-piano-efi-entry-debug+`；Image有有效ARM64/AA64 PE header，SizeOfImage仍40501248，PE entry RVA为1DE55D4，primary_entry RVA仍1D3F0F0。构建使用隔离O=`build/kernel-topics/piano-efi-entry-debug`与本机LLVM23.1.1，完整Image及最终增量均exit0且无warning。其构建时间和version counter照实记录于manifest，本轮未声称byte-for-byte重建复现。

六个阶段为`PIANO_EFI_ENTER_KERNEL`（附真实entry/fdt/size）、`PIANO_PRIMARY_ENTRY`、`PIANO_EFI_INIT_BEFORE`、`PIANO_RESERVE_REGIONS_BEFORE`、`PIANO_RESERVE_REGIONS_AFTER`、`PIANO_EFI_INIT_AFTER`。stub使用firmware identity mapping；首条primary_entry的asm不使用stack或BL，只改x9–x17/NZCV，21字节固定loop，签名/start<capacity/size<=capacity/start<=size失败均继续原boot。数据先clean/invalidate至PoC，再发布header并同步。内核C阶段已经移除firmware idmap，使用early_memremap header12bytes及每段≤128bytes数据，映射失败返回，不推进header；没有allocator、BS、磁盘、module或设备MMIO。early_memremap的页表来自静态fixmap，同属性WB重叠映射符合ARM64 D-cache PIPT契约。

实际源码host测试用ASan/UBSan覆盖wrap/saturation、错误签名、capacity越界、start>size、零/超长marker和header/data/第二片映射失败。最终生成汇编由独立agent复核；导出文件为`primary-entry.asm.txt`、`efi-stub-arm64.asm.txt`、`kernel-marker.asm.txt`。checkpatch（no-tree/no-signoff/strict，仅忽略FILE_PATH_CHANGES）为0errors/0warnings/0checks。

额外default-off编译保存于`build/kernel-topics/piano-efi-entry-default-off`，四处关键代码section与官方7704对象逐字节相同：head `.idmap.text`1508bytes、stub `.text`348bytes、setup `.init.text`2176bytes、EFI init `.init.text`1308bytes。证据与hash见产物`default-off-proof.json`。这验证本次未启用marker时这些执行路径的代码没有变化，不是全Image或全部架构的等价证明。

本轮未执行设备操作。marker仍可能因ring拒绝、pstore后续archive/reset或恢复覆盖而缺失，不能把缺marker解释为没有执行。旧next initramfs-manifest的Image/config/source绑定已不匹配此debug topic。现已通过独立严格入口生成新诊断RAM CPIO/V2 payload，绑定094d topic/Image/config/同一captured live.dtb，详见[诊断包装说明](linux-diagnostic-payloads.md)；没有重标旧manifest或移动日常stable/next pins。root需在独立目录验证后显式接入下一次controlled comparison，不引入overlay/rootfs变化。


## test81：已测入口和EFI内存重建均完成

固化证据为`artifacts/kernel-topics/piano-efi-entry-debug/efi-validation-test-81.json`，boot image SHA72cef1a8379e71ec7c6bae8badc5be8fa4475ea042d338fbdc0968b5a69086a4。payload/Image/initrd/DTB均与诊断manifest完全匹配，EBS实际success。console按顺序出现全部六个marker：EFI_ENTER_KERNEL → PRIMARY_ENTRY → EFI_INIT_BEFORE → RESERVE_REGIONS_BEFORE → RESERVE_REGIONS_AFTER → EFI_INIT_AFTER。实际entry为D053F0F0、final FDT为CE550000、传入size10EE88，补足test78仅按LoaderData布局推断的地址。

最后map仍49descriptors，严格usable WB为402337792bytes=383.69921875MiB，比test78少8KiB。记录这些差异不代表内存容量是故障原因。六marker证明已进入主线并越过EFI内存重建及efi_init返回；尚未证明随后arm64_memblock_init/paging/ramoops或PID1成功。回收文件仅console-ramoops-0（2097140bytes，SHA1412d98dddad0245da26d9ce7fd41418235a4f574e9762d928744d566918afe0），无dmesg/pmsg。本次主线printk banner/命令行/PID1均缺失；console前面的Call trace来自旧Android历史，不能当成本次主线panic栈。Android自动恢复，26boot hashes/root/A槽正常。

094d入口topic作为已测阶段基线保留。后续独立`topic/piano-efi-memory-debug`从094d追加源码，只增加有限阶段与早期printk/panic回收，硬件仍待root验收；不修改平台完整DRAM map、不移动stable/next/旧094d策略，也不预断RAM不足。
