# UFS 专用测试空间离线审计

2026-10-05。只读取 `private/analysis/ufs-gpt-android-test-57/` 的六份主 GPT header/entry array 与 manifest，并重新解析、计算 CRC 和区间；本次没有读取或写入设备，没有修改源码。

## 结论

**存在五处真实的 GPT 未分配尾隙，全部避开现有 133 个活动分区；LUN0 没有未分配 gap。**其中最大 1 MiB 对齐候选在 **LUN4 / LBA 375040–378623，14,680,064 bytes / 14 MiB**。创建此范围无需移动、缩小或改名任何现有活动分区。

现有快照仍不足以把候选宣告为已保留的 PianoUEFI 专用区：它只保存主 GPT，没有备 GPT、gap 原始内容备份、同机 rawprogram/patch 分区配置与原始 LBA 引用审计，也未确认候选 LUN 的写保护与缓存/FUA 能力。下面列出的候选均是可审查的规划，不是已授权的 GPT 或 UFS 写入。

## 计算依据

所有 LUN 逻辑块均为 **4096 bytes**。按 TypeGUID 非零确定活动条目，检查每条 `FirstUsable <= start <= end <= LastUsable`，按 start 排序后验证每条后继 `start == 前条 end+1`。六个 LUN 的活动条目分别覆盖从 FirstUsable 起的连续区间，因此仅有表中尾隙，没有内部间隙或活动分区重叠。所有六份主 header CRC 与 entry-array CRC 均重新通过，且与 manifest 的 Android/UEFI 对照值匹配；这是测试 57 快照的离线事实，不是今天的实时设备状态。

[UEFI GPT 规范](https://uefi.org/specs/UEFI/2.11/05_GUID_Partition_Table_Format.html) 定义可用 LBA 的闭区间，以及 TypeGUID 为零的未使用条目。空 GPT 条目只是创建分区的目录槽位，不会额外产生磁盘空间。

| LUN / 快照设备 | 容量 bytes | 可用 LBA 闭区间 | GPT 条目 / 活动 | array LBA / blocks | 主 header CRC / array CRC |
| --- | ---: | --- | --- | --- | --- |
| 0 / sda | 510,165,778,432 | 6–124552186 | 64 / 34 | 2 / 2 | `8B7FD0E1` / `03A3A74E` |
| 1 / sdb | 12,582,912 | 6–3066 | 32 / 5 | 2 / 1 | `AB3E32B6` / `C39A16B2` |
| 2 / sdd | 12,582,912 | 6–3066 | 32 / 5 | 2 / 1 | `FB91BB42` / `0135E175` |
| 3 / sdc | 8,388,608 | 6–2042 | 32 / 3 | 2 / 1 | `8E60BBE1` / `0497451F` |
| 4 / sde | 1,551,892,480 | 6–378872 | 96 / 79 | 2 / 3 | `115C4E81` / `0540A9DA` |
| 5 / sdf | 67,108,864 | 6–16378 | 32 / 7 | 2 / 1 | `272830A4` / `AC95B544` |

快照中设备字母与 LUN 的对应来自 manifest；例如 LUN2 是 sdd，LUN3 是 sdc，不能按字母顺序推算。头部 `LBA 0–5` 全部排除；尾部排除 `LastUsable+1–AlternateLBA`。备 header 的指向虽然已核对等于设备末 LBA，备 header/array 自身尚未读取校验。

| LUN | 活动分区占用的连续闭区间 | 排除的末尾 GPT/保留 LBA |
| --- | --- | --- |
| 0 | 6–124552186 | 124552187–124552191 |
| 1 | 6–2261 | 3067–3071 |
| 2 | 6–2261 | 3067–3071 |
| 3 | 6–575 | 2043–2047 |
| 4 | 6–374923 | 378873–378879 |
| 5 | 6–14847 | 16379–16383 |

## 尾隙和对齐候选

每处 raw gap 自身已是 4 KiB 逻辑块对齐。1 MiB 版本把起点向上取整到 256 LBA 倍数、把终点后一 LBA 向下取整到同样边界；以下完整范围不会跨越任何活动分区。1 MiB 对齐仅是明确的工程边界，不能证明设备的 NAND 擦除粒度或断电写原子性。

| LUN | 原始 gap LBA 闭区间 | blocks | 原始 bytes | 1 MiB 对齐 LBA 闭区间 | 对齐 bytes / MiB |
| --- | --- | ---: | ---: | --- | --- |
| 1 | 2262–3066 | 805 | 3,297,280 | 2304–2815 | 2,097,152 / 2 |
| 2 | 2262–3066 | 805 | 3,297,280 | 2304–2815 | 2,097,152 / 2 |
| 3 | 576–2042 | 1467 | 6,008,832 | 768–1791 | 4,194,304 / 4 |
| 4 | 374924–378872 | 3949 | 16,175,104 | 375040–378623 | 14,680,064 / 14 |
| 5 | 14848–16378 | 1531 | 6,270,976 | 14848–16127 | 5,242,880 / 5 |

## 空条目和 last_parti

五个尾隙恰好对应一个 **TypeGUID 全零而其余字段非零**的 `last_parti` 条目。每个 marker 有 UniqueGUID、范围及 attributes `0x1000000000000000`，因此不能把整条目误报为全零，也不应根据名称认定它是活动分区或隐藏数据。

Qualcomm 官方 [qcom-ptool UFS 示例](https://github.com/qualcomm-linux/qcom-ptool/blob/main/uefi/platforms/qcs9100-ride-sx/ufs/partitions.conf) 明确设置同名、零 TypeGUID、初始 size=0 的末条目；[ptool 的 grow-last 逻辑](https://github.com/qualcomm-linux/qcom-ptool/blob/main/qcom_ptool/ptool.py) 将末条目补到可用范围末端。因此现象符合 Qualcomm 的未分配尾部占位方式；该示例不是本机分区生成配置，不能替代本机 rawprogram/固件使用情况核对。

条目序号均为 **1-based**；每条占 128 bytes。以下全部空槽总计 155 个，其中完全全零 150 个，另 5 个为上述 marker。

| LUN | TypeGUID=0 的条目数 | 非全零 marker：槽位 / 闭区间 | 完全全零槽位 | 完全全零条目 bytes |
| --- | ---: | --- | --- | ---: |
| 0 | 30 | 无 | 35–64 | 3,840 |
| 1 | 27 | 6 / 2262–3066 | 7–32 | 3,328 |
| 2 | 27 | 6 / 2262–3066 | 7–32 | 3,328 |
| 3 | 29 | 4 / 576–2042 | 5–32 | 3,584 |
| 4 | 17 | 80 / 374924–378872 | 81–96 | 2,048 |
| 5 | 25 | 8 / 14848–16378 | 9–32 | 3,072 |

## 可命名/保留的规划边界

LUN4 的 `375040–378623` 是最大对齐候选，可规划名称 **PianoUEFI-Test**，采用新生成的专用 TypeGUID 和 UniquePartitionGUID，以 GUID、LUN、capacity、完整区间共同匹配；完全全零槽 **81** 可容纳新条目，原 79 个活动条目的内容可以逐字节保持。槽 80 的 `last_parti` 处理必须在具体 GPT 计划中说明，不能未经审查删除或把它转换成新用途；尤其不能让旧 Qualcomm grow-last 工具在后续重新生成时覆盖新的定义。

“新增命名 GPT 分区”仍要修改 **主/备 entry array 和相应 header CRC**。这与“不移动或改写现有分区”是两个不同的事实。首次原始 4 KiB 写入实验可以先使用明确批准的 LUN/LBA allowlist 而不改 GPT；真正长期 FAT/变量区应有持久、可审查的保留定义，不能依赖一段只存在于代码中的裸 LBA。

另外两项现有分区明确排除：LUN0 `logfs` 为 LBA **4096–6143 / 8,388,608 bytes**；LUN4 `uefivarstore` 为 **361362–361489 / 524,288 bytes**。它们都是活动分区，名称不能构成接管授权。LUN0 `userdata` 从 3632128 一直覆盖 LastUsable=124552186，文件系统内部剩余空间不是 GPT gap。本记录没有提出或实施缩小 userdata。

## 首次 UFS 写入的备份、同步与恢复边界

当前实机只读路径 `PianoReadOnlyBlock.c` 无 WRITE CDB，FlushBlocks 的成功只是只读无操作，不是设备同步。早期 `PianoUfsBlockIo.c` 有 WRITE(16) FUA 和 SYNCHRONIZE CACHE(10)，但没有接到当前 owned-DMA 只读 transport；它的主机测试不能当作物理写入验证。

早期 helper 的 First>=34、Last<=LastBlock-33 仅是固定头尾保护，既没有解析本机主/备 GPT，也没有排除已有活动分区；它不能单独构成允许写范围。新 transport 必须在完整 GPT 校验后执行明确的 LUN/闭区间 allowlist，并在拆 chunk 之前拒绝任何跨界请求。

首写方案应限定为以下具体顺序，全部证据完成后才允许扩大范围或引入 FAT：

1. 确认实时主、备 GPT 和 LUN capacity 与审核快照一致，并审核 gap 的平台使用；明确批准一个 LUN 和一个 **完整 4096-byte block**。不修改 UFS provisioning、descriptor、write-protect 配置、RPMB，不发送 UNMAP/FORMAT/erase。
2. 同一个 LBA 读取两次，用独立 DMA/CPU 接收缓冲校验一致；将原块与完整主/备 GPT 备份保存到设备外的持久存储，记录 LUN、LBA、大小、SHA-256。必须校验可读回的备份，不能只在将重启的 RAM 中保存恢复副本。此处当前只有 GPT 主副本，没有任何 gap 原块备份。
3. 使用精确 CDB、UPIU WRITE flag、UTRD host-to-device 和 DMA to-device 生命周期；在发第一块之前检查整个请求区间，只允许该块。等待 doorbell、OCS、UPIU response、SCSI GOOD、tag 和 residual/传输长度均成功。
4. 若实际 LUN 的 MODE SENSE/cache 能力支持 FUA，WRITE(10)/(16) byte 1 bit 3 设置 `0x08`；写后执行同步 cache 并严格检查成功。若不支持 FUA，使用已确认的 write+cache-sync 持久化方式，不能忽略 ILLEGAL REQUEST 或把“不支持”当成通过。实际缓存能力尚未收集。[Linux 缓存/FUA 契约](https://docs.kernel.org/block/writeback_cache_control.html) 与 [SCSI sd 实现](https://github.com/torvalds/linux/blob/master/drivers/scsi/sd.c) 是实现参照。
5. 重新发 READ 到独立接收缓冲，invalidate/sync 后逐字节比较测试内容；CPU memcpy 或同一 bounce buffer 的残留不能充当设备读回。普通读回匹配也不能独自证明断电持久化；FUA/sync 不是多块事务或 4 KiB 原子性证明。
6. 无论测试比较成功与否，只要控制器/命令状态已明确安全，就写回备份原块，执行相同持久化同步，独立重读校验 SHA-256 完全恢复；同时重新验证 GPT 和邻近受保护基线。存在 timeout 或未知写完成状态时，先排除在途 DMA/命令，再决定恢复，不在旧命令可能仍运行时重用缓冲或并发恢复。[Linux UFS command data direction](https://github.com/torvalds/linux/blob/master/drivers/ufs/core/ufshcd.c#L2596)
7. 恢复确认后才重启；重启后由独立只读路径再次核对原块和主/备 GPT。若恢复不能确认，保留日志、备份与不确定状态，停止后续测试，不能宣称零改动。

保存主/备 GPT 是恢复输入，不意味着 GPT 改写天然安全；FUA/flush 只处理数据持久化，也不修复错误 LUN、错误 LBA 或错误保护区判断。

## FAT 与变量落地

专用区与单块 write/restore 验证完成后，才能规划 FAT image。整个文件系统写集合含 BPB、FAT、副本、目录和数据，都必须约束在候选范围，挂载/format 也算写入。先在主机/RAM image 验证格式、再只读挂载确认，最后执行授权的限定写入、FlushBlocks 与独立读回。文件 close/应用报告成功不能替代设备同步。

14 MiB 小区并不是合规 FAT32 的保证。以当前 4096-byte 逻辑 sector、每 cluster 至少一 sector 计算，全卷最多 3584 clusters，低于 FAT16 起点 4085；常规格式会采用 **FAT12**。512-byte BPB 的 FAT16 方案需验证 DiskIo 4096-byte 读改写边界；不应直接强制 FAT32。本仓库 `FatPkg/EnhancedFatDxe/Init.c` 会拒绝 clusters 太少的 FAT32，阈值见 `FatFileSystem.h`；[EDK2 FAT 检测实现](https://github.com/tianocore/edk2/blob/master/FatPkg/EnhancedFatDxe/Init.c)。测试数据分区与 ESP 的格式/引导目标需分别明确，不凭“FAT”即宣称可作为启动 ESP。

当前 `pianoGui.dsc` 明确启用 `PcdEmuVariableNvModeEnable|TRUE`，VariableRuntimeDxe 的 EmuNvMode 写 RAM，不提供此次 UFS 非易失落地。FAT 文件保存只能首先证明 boot-services 阶段的应用持久化；`SetVariable(NV)` 的成功、跨重启读取及运行时能力是后续独立验收。[UEFI 变量属性](https://uefi.org/specs/UEFI/2.11/03_Boot_Manager.html#globally-defined-variables)

真正 NV 后端要定义掉电恢复格式与提交顺序，例如分离数据/commit 的双副本、generation、CRC、容量与回收；若沿用 EDK2 VariableRuntimeDxe，则要完成相应 FVB/FTW 语义适配，而不是把某个 FAT 文件名当作现有 flash store。当前 UFS ExitBootServices 路径 halt 控制器，故不能直接承诺 ExitBootServices 后仍可从 runtime SetVariable 写 UFS；需单独设计 runtime 资源/同步或明确只支持前启动持久化。

## 全部活动 GPT 分区区间

下面逐项列出离线解析的 133 个活动分区，LBA 为闭区间，bytes=(end-start+1)*4096。此表不包含 TypeGUID=0 的 marker；marker 已单列。

| LUN / 槽位 | 名称 | 起点 LBA | 终点 LBA | bytes |
| --- | --- | ---: | ---: | ---: |
| 0 / 1 | switch | 6 | 7 | 8,192 |
| 0 / 2 | ssd | 8 | 15 | 32,768 |
| 0 / 3 | dbg | 16 | 23 | 32,768 |
| 0 / 4 | bk01 | 24 | 31 | 32,768 |
| 0 / 5 | secinfo | 32 | 63 | 131,072 |
| 0 / 6 | bk03 | 64 | 127 | 262,144 |
| 0 / 7 | bk04 | 128 | 255 | 524,288 |
| 0 / 8 | keystore | 256 | 383 | 524,288 |
| 0 / 9 | frp | 384 | 511 | 524,288 |
| 0 / 10 | countrycode_a | 512 | 767 | 1,048,576 |
| 0 / 11 | countrycode_b | 768 | 1023 | 1,048,576 |
| 0 / 12 | misc | 1024 | 2047 | 4,194,304 |
| 0 / 13 | bk05 | 2048 | 4095 | 8,388,608 |
| 0 / 14 | logfs | 4096 | 6143 | 8,388,608 |
| 0 / 15 | ffu | 6144 | 8191 | 8,388,608 |
| 0 / 16 | vm-persist | 8192 | 17919 | 39,845,888 |
| 0 / 17 | vm-bootsys_a | 17920 | 23039 | 20,971,520 |
| 0 / 18 | vm-bootsys_b | 23040 | 28159 | 20,971,520 |
| 0 / 19 | mbnconfig | 28160 | 36351 | 33,554,432 |
| 0 / 20 | metadata | 36352 | 52735 | 67,108,864 |
| 0 / 21 | devinfo | 52736 | 52737 | 8,192 |
| 0 / 22 | vbmeta_system_a | 52738 | 52769 | 131,072 |
| 0 / 23 | vbmeta_system_b | 52770 | 52801 | 131,072 |
| 0 / 24 | bk07 | 52802 | 54015 | 4,972,544 |
| 0 / 25 | charger | 54016 | 54271 | 1,048,576 |
| 0 / 26 | blackbox | 54272 | 96255 | 171,966,464 |
| 0 / 27 | oops | 96256 | 100351 | 16,777,216 |
| 0 / 28 | rawdump | 100352 | 177151 | 314,572,800 |
| 0 / 29 | opconfig | 177152 | 182271 | 20,971,520 |
| 0 / 30 | mem | 182272 | 183295 | 4,194,304 |
| 0 / 31 | mtdblk | 183296 | 191487 | 33,554,432 |
| 0 / 32 | rescue | 191488 | 224255 | 134,217,728 |
| 0 / 33 | super | 224256 | 3632127 | 13,958,643,712 |
| 0 / 34 | userdata | 3632128 | 124552186 | 495,288,561,664 |
| 1 / 1 | xbl_a | 6 | 2053 | 8,388,608 |
| 1 / 2 | xbl_config_a | 2054 | 2181 | 524,288 |
| 1 / 3 | multiimgqti_a | 2182 | 2189 | 32,768 |
| 1 / 4 | multiimgoem_a | 2190 | 2197 | 32,768 |
| 1 / 5 | apdp | 2198 | 2261 | 262,144 |
| 2 / 1 | xbl_b | 6 | 2053 | 8,388,608 |
| 2 / 2 | xbl_config_b | 2054 | 2181 | 524,288 |
| 2 / 3 | multiimgqti_b | 2182 | 2189 | 32,768 |
| 2 / 4 | multiimgoem_b | 2190 | 2197 | 32,768 |
| 2 / 5 | apdpb | 2198 | 2261 | 262,144 |
| 3 / 1 | ALIGN_TO_128K_1 | 6 | 31 | 106,496 |
| 3 / 2 | cdt | 32 | 63 | 131,072 |
| 3 / 3 | ddr | 64 | 575 | 2,097,152 |
| 4 / 1 | uefi_a | 6 | 1285 | 5,242,880 |
| 4 / 2 | idmanager_a | 1286 | 1413 | 524,288 |
| 4 / 3 | aop_a | 1414 | 1541 | 524,288 |
| 4 / 4 | aop_config_a | 1542 | 1669 | 524,288 |
| 4 / 5 | tz_a | 1670 | 2949 | 5,242,880 |
| 4 / 6 | hyp_a | 2950 | 4997 | 8,388,608 |
| 4 / 7 | modem_a | 4998 | 56709 | 211,812,352 |
| 4 / 8 | bluetooth_a | 56710 | 58757 | 8,388,608 |
| 4 / 9 | abl_a | 58758 | 60805 | 8,388,608 |
| 4 / 10 | dsp_a | 60806 | 77189 | 67,108,864 |
| 4 / 11 | keymaster_a | 77190 | 77317 | 524,288 |
| 4 / 12 | spuservice_a | 77318 | 77349 | 131,072 |
| 4 / 13 | boot_a | 77350 | 101925 | 100,663,296 |
| 4 / 14 | devcfg_a | 101926 | 101989 | 262,144 |
| 4 / 15 | qupfw_a | 101990 | 102021 | 131,072 |
| 4 / 16 | vbmeta_a | 102022 | 102053 | 131,072 |
| 4 / 17 | dtbo_a | 102054 | 108197 | 25,165,824 |
| 4 / 18 | uefisecapp_a | 108198 | 108709 | 2,097,152 |
| 4 / 19 | imagefv_a | 108710 | 120997 | 50,331,648 |
| 4 / 20 | shrm_a | 120998 | 121061 | 262,144 |
| 4 / 21 | cpucp_a | 121062 | 121317 | 1,048,576 |
| 4 / 22 | featenabler_a | 121318 | 121349 | 131,072 |
| 4 / 23 | vendor_boot_a | 121350 | 145925 | 100,663,296 |
| 4 / 24 | qmcs | 145926 | 153605 | 31,457,280 |
| 4 / 25 | qweslicstore_a | 153606 | 153669 | 262,144 |
| 4 / 26 | recovery_a | 153670 | 179269 | 104,857,600 |
| 4 / 27 | xbl_ramdump_a | 179270 | 179781 | 2,097,152 |
| 4 / 28 | init_boot_a | 179782 | 181829 | 8,388,608 |
| 4 / 29 | cpucp_dtb_a | 181830 | 181845 | 65,536 |
| 4 / 30 | pvmfw_a | 181846 | 182101 | 1,048,576 |
| 4 / 31 | soccp_debug_a | 182102 | 182229 | 524,288 |
| 4 / 32 | soccp_dcd_a | 182230 | 182235 | 24,576 |
| 4 / 33 | pdp_a | 182236 | 182299 | 262,144 |
| 4 / 34 | pdp_cdb_a | 182300 | 182331 | 131,072 |
| 4 / 35 | uefi_b | 182332 | 183611 | 5,242,880 |
| 4 / 36 | idmanager_b | 183612 | 183739 | 524,288 |
| 4 / 37 | aop_b | 183740 | 183867 | 524,288 |
| 4 / 38 | aop_config_b | 183868 | 183995 | 524,288 |
| 4 / 39 | tz_b | 183996 | 185275 | 5,242,880 |
| 4 / 40 | hyp_b | 185276 | 187323 | 8,388,608 |
| 4 / 41 | modem_b | 187324 | 239035 | 211,812,352 |
| 4 / 42 | bluetooth_b | 239036 | 241083 | 8,388,608 |
| 4 / 43 | abl_b | 241084 | 243131 | 8,388,608 |
| 4 / 44 | dsp_b | 243132 | 259515 | 67,108,864 |
| 4 / 45 | keymaster_b | 259516 | 259643 | 524,288 |
| 4 / 46 | spuservice_b | 259644 | 259675 | 131,072 |
| 4 / 47 | boot_b | 259676 | 284251 | 100,663,296 |
| 4 / 48 | devcfg_b | 284252 | 284315 | 262,144 |
| 4 / 49 | qupfw_b | 284316 | 284347 | 131,072 |
| 4 / 50 | vbmeta_b | 284348 | 284379 | 131,072 |
| 4 / 51 | dtbo_b | 284380 | 290523 | 25,165,824 |
| 4 / 52 | uefisecapp_b | 290524 | 291035 | 2,097,152 |
| 4 / 53 | imagefv_b | 291036 | 303323 | 50,331,648 |
| 4 / 54 | shrm_b | 303324 | 303387 | 262,144 |
| 4 / 55 | cpucp_b | 303388 | 303643 | 1,048,576 |
| 4 / 56 | featenabler_b | 303644 | 303675 | 131,072 |
| 4 / 57 | vendor_boot_b | 303676 | 328251 | 100,663,296 |
| 4 / 58 | qweslicstore_b | 328252 | 328315 | 262,144 |
| 4 / 59 | recovery_b | 328316 | 353915 | 104,857,600 |
| 4 / 60 | xbl_ramdump_b | 353916 | 354427 | 2,097,152 |
| 4 / 61 | init_boot_b | 354428 | 356475 | 8,388,608 |
| 4 / 62 | cpucp_dtb_b | 356476 | 356491 | 65,536 |
| 4 / 63 | pvmfw_b | 356492 | 356747 | 1,048,576 |
| 4 / 64 | soccp_debug_b | 356748 | 356875 | 524,288 |
| 4 / 65 | soccp_dcd_b | 356876 | 356881 | 24,576 |
| 4 / 66 | pdp_b | 356882 | 356945 | 262,144 |
| 4 / 67 | pdp_cdb_b | 356946 | 356977 | 131,072 |
| 4 / 68 | toolsfv | 356978 | 357233 | 1,048,576 |
| 4 / 69 | gsort | 357234 | 361329 | 16,777,216 |
| 4 / 70 | storsec | 361330 | 361361 | 131,072 |
| 4 / 71 | uefivarstore | 361362 | 361489 | 524,288 |
| 4 / 72 | secdata | 361490 | 361497 | 32,768 |
| 4 / 73 | mdcompress | 361498 | 366617 | 20,971,520 |
| 4 / 74 | connsec | 366618 | 366649 | 131,072 |
| 4 / 75 | tzsc | 366650 | 366681 | 131,072 |
| 4 / 76 | spunvm | 366682 | 374873 | 33,554,432 |
| 4 / 77 | xbl_sc_test_mode | 374874 | 374889 | 65,536 |
| 4 / 78 | xbl_sc_logs | 374890 | 374921 | 131,072 |
| 4 / 79 | dpm | 374922 | 374923 | 8,192 |
| 5 / 1 | ALIGN_TO_128K_2 | 6 | 31 | 106,496 |
| 5 / 2 | bk51 | 32 | 255 | 917,504 |
| 5 / 3 | modemst1 | 256 | 2303 | 8,388,608 |
| 5 / 4 | modemst2 | 2304 | 4351 | 8,388,608 |
| 5 / 5 | fsg | 4352 | 6399 | 8,388,608 |
| 5 / 6 | fsc | 6400 | 6655 | 1,048,576 |
| 5 / 7 | persist | 6656 | 14847 | 33,554,432 |

## 证据完整性

原始目录每个 LUN 只有 `mbr-header.bin`（8192 bytes）和 `entries.bin`（按 header 数量保存的实际 entry array），没有 backup-header/backup-array 或 gap 数据。下表是本次独立计算的 SHA-256，以便后续确认分析输入没有变化。

| LUN | mbr-header.bin SHA-256 | entries.bin SHA-256 |
| --- | --- | --- |
| 0 | `a18e9cb3728f41c623d6c246629f1858464fa3c83d92de05a97edefd8409fb74` | `7fd94fdacb631fb78ea68b6039ac6c0204f68a576127161ddf09456d3945559b` |
| 1 | `d83e8f918e602086d1b75c3e388689713b6086c7276b66087cde62ccb807a1de` | `89c2a76b19bd6efccfc9bb0bc25c4e7ae9e86cc36e7a1d378cdb48ec83a9e7b8` |
| 2 | `3d885da67d576efe7cf37414c9cb889dd0944181246f127b2f8b477df50b5a0e` | `a3e355ebce87761e7fa30fc24d6135811854e4f02f597c2ab5c63b4d13f4aff9` |
| 3 | `c112324aedb0882cebdcb8d26bff61d879d950627ebeb781fd3685a4e3b58b1b` | `43504c4720735b31cc539f00f396f63f9494694a56e2b7a89a47ff01e1551187` |
| 4 | `972c4da78511c106d717f34cdefd24f4bebb1848ce9216d18a938e3121937c2d` | `fd10bb4f7142eccb28a6acc5fc6e928c599883fb44a0338b521473c5669a3506` |
| 5 | `e4ac4790191d23d282144d7f6d2df3f2f5496432ff8f5b15d73747174d5deb0d` | `dfae705f847490eae3576aa5d1b0328d483186795c20de8ded30369e21467961` |
