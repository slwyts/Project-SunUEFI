# 单一PianoUEFI-product shared core的最小OSExit合同

目标只有一个PianoUEFI-product.img，fastboot_boot/boot/recovery入口仅选择策略；runtime/supervisor/USB/输入/GUI/filesystem使用同一core和同一owner ledger。本文件是最小协作合同，不增加功能profile或image分支。71/73的两个exact kernel pins作为rescue数据保留，research候选不移动它们。

## UEFI阶段持续服务

所有UEFI阶段同一fastboot实例保持可发现；Shell/Setup/GUI/返回EFI app通过shared runtime pump协作。timer/notify只置标志或发布预分配工作，不在CALLBACK执行Fetch/BlockIO/GOP/LoadImage。actual TPL_APPLICATION、非reentry、BSalive时才由单实例supervisor dispatch一小工作slice；Budget是admission，不伪称能中断阻塞HAL。

核心所需boot services为protocol/event/pool/image服务及有效完整GetMemoryMap；输入/文件系统/GOP仍走已验证owner。RuntimeServices最少真实cold ResetSystem和可明确区分boot-only volatile数据的variable服务。time/其他runtime接口只按实际支持声明，不用存在的vtable指针认定实机可用。

产品所谓“全UEFI后台”覆盖BS live阶段；成功ExitBootServices后进入OS ownership，不能让旧BootServices USB pump、Shell、BlockIO、GOP或native boot-driver callback继续执行。[UEFI官方overview](https://uefi.org/specs/UEFI/2.10_A/02_Overview.html)规定该边界及runtime memory保留；[PI DXE driver规则](https://uefi.org/specs/PI/1.8A/V2_DXE_Drivers.html)明确runtime driver也不能调用已退出的BS/DXE/boot-driver服务。

## PrepareOsExit在最后map之前完成

同一transition lease阻止新UI action/命令和storage open，冻结dispatch、完成已复制ACK/TX queue，等待当前bounded worker退出。然后由每个owner给exact成功退休证明：

- USB：halt、全部DMA idle/unmap/free、owned domain/table/maps0、所有stream/peer/display合法、8clockmask/GDSC释放及shutdown协议撤销。
- UFS：断开消费者与filesystem/BlockIO、halt/doorbell/run/bases、DMA/domain、published protocols、clock各阶段报告。现reset-only两阶段合同不能直接冒充OSExit；对OS必须另验证“已退役”或明确保留给Linux接管。
- 输入/GUI和timer：禁新工作，停止callbacks，归还pending loan；cached protocol/boot code pointer不得跨边界调用。

Root/BaseKernel/initrd/完整DTB和source blob的owner/释放顺序写清。供OS继续使用的内存保留有依据的EFI类别与DT reserved-memory，禁止释放仍有DMA refs的页或在EBS callback中allocate/free、clock/HAL/Protocol调用。unknown进入quarantine并拒OS启动，不拿halt-only成功代替所有权清理。

停止/退休后才获取最终map/key。full DRAM/固定no-map/CMA/firmware/cache contracts必须正确；当前52desc修复了fixed CMA的pfn fault，但常规RAM不足仍导致88 OOM，rc6版本变化不能解决该map问题。Kernel直接从EFI rebuild RAM，因此不能以raw71/73的15GiB成功代替标准EFI memmap验收。

## EBS一次进入后的约束

actual Mu CoreExitBootServices在DxeMain.c:789发Before事件，797关timer，802校验/terminate map；失败发Failed事件并返回，成功816发Exit事件，841..858清SystemTable的BS/console和BootServices table。现实现第一次失败也可能已经停timer；[UEFI官方boot-services规则](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)允许partial shutdown和新map/key重试。因此不能在第一次EBS尝试失败后自动恢复GUI/background service或再启动另一个image；保持handoff lease冻结，只做受控map/EBS重试或失败封闭恢复。

Exit notify只写CPU flag/已预分配证据。成功后所有clients永久拒cached provider callbacks；普通cleanup/CloseEvent/FreePool/Debug依赖BS的路径不得运行。runtime Reset/time/RT-variable只能在声明支持和正确runtime mappings下使用；boot-only Probe变量在OS后不当作持续service。runtime metadata/map必须保留正确权限/cache，不能给OS尚未证明的full DRAM容量或继续使用native HAL。

返回EFI app且未EBS时可在exact image/unload/options/loan/source释放完成后由supervisor建立下一service session并resume同一product实例；发生EBS则进入不可恢复到UEFI UI的terminal phase，不能按普通StartImage返回处理。現Root+Launch两道EBS fence正是该fail-closed边界，不能因UI action或新入口绕开。
