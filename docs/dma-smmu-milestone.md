# piano SM8750 DMA / SMMU milestone

当前目标：统一 DMA/SMMU → UFS NOP / QUERY → REPORT LUNS / READ CAPACITY → 只读 LBA / GPT。USB bulk、BlockIO、触摸真实输入在此目标完成后再接入。当前固件不链接块写入实现，不提供 WRITE、UNMAP、FORMAT、RPMB、WRITE DESCRIPTOR、WRITE ATTRIBUTE 或 SET FLAG 构造器。

## DMA 内存和地址

| 项目 | 已核对的值 / 约束 |
| --- | --- |
| DMA 分配来源 | 当前 EDK2 `DXE_Heap`，PA `0xBD930000`–`0xD7FFFFFF`；通过 BootServices 分配自身拥有的页 |
| 类型 | `EfiBootServicesData`，GCD 必须为 SystemMemory，属性必须有 WB |
| CPU 地址 | 每一页通过 `AT S1E1R / PAR_EL1` 重新查询，确认 CPU VA 与分配 PA 的关系 |
| 基本对齐 | 每个 buffer 独占完整 4 KiB 页；设备要求和缓存对齐不得破坏页边界 |
| 当前缓存行 | 64 字节；缓存操作覆盖独占页的完整范围 |
| UTRDL / UCD | 最低 1024 / 128 字节对齐，统一分配器实际提升为 4 KiB |
| 设备地址 | 当前自有 IOVA 区间 `0x40000000`–`0x40FFFFFF`，16 MiB，绝不默认 PA=IOVA |
| 地址位数 | 分配器分别约束 CPU PA 和设备地址的 32 / 64 位上限；当前 UFS 实机使用 32 位范围 |
| SMMU PA 位数 | 自有 CB 的 TCR2.PASIZE 为 36 位；后端拒绝超过 36 位的 PA |
| 页表 | 4 KiB granule，三级 AArch64 stage-1，39 位输入；10 页 / 40 KiB 表内存 |

framebuffer、ramoops、内核载荷、原厂启动器留下的命令列表及其他 reserved-memory 均不作为 DMA 分配池。CPU 页表与设备 IOMMU 页表分别核对。

## 当前启动器留下的 SMMU

数据来自同机 DT 和第 39 / 40 次实机只读快照，不能当作所有 SM8750 设备的通用常量。

| 项目 | 结果 |
| --- | --- |
| apps SMMU | v500，基址 `0x15000000`，DT 窗口 `0x100000` |
| ID0 / ID1 / ID2 | `4C017E7F` / `60000053` / `00005111` |
| 资源 | 127 个 SMR group，83 个 context bank，4 KiB register page，CB 区偏移 `0x80000` |
| 未匹配流 | 默认 fault；不能假设 bypass 或 identity DMA |
| UFS | SID `0x60`，原槽 0 无 valid，S2CR 为 fault |
| USB DWC3 | SID `0x40`，原槽 3 无 valid，S2CR 为 fault |
| GPI / QUP SE | DT SID `0xB6` / `0xA3`，尚未建立并验证设备专用 DMA 上下文 |
| 显示 | SID `0x800` / mask `2`，原槽 2 有效；保持原上下文 |

`PianoSmmu` 只读 SMR / S2CR、有效 CB 的 SCTLR / TTBR / TCR / MAIR 和 fault。重复匹配、窗口越界、DT 不符会拒绝继续。

`PianoOwnedSmmu` 当前只允许 SID60。它核对同机捕获的 HALIOMMU 协议版本、模块内 API 地址和函数表，通过 `UFS_MEM` 单独创建 domain / attach。没有调用全局 detach 或重置其他上下文。CB0 的 TTBR0 指向自己分配的页表；SCTLR / TCR / TCR2 / MAIR 必须读回匹配后才能映射。解除时再验证 UFS 流消失、其他 SMR / S2CR 不变，验证失败不释放表内存。

## 统一接口

实现：`bootprofiles/uefi-app/PianoDma.c/.h`。

1. `PianoDmaAllocate`：分配独占页，核对 PA、WB 和地址位数。
2. `PianoDmaMap`：调用设备后端建立映射，检查 IOVA 范围与对齐；无后端返回 NotReady。
3. `PianoDmaPhysicalToDevice / PianoDmaDeviceToPhysical`：在已映射 buffer 的真实有效字节内转换，拒绝页尾 padding、越界和隔离状态。
4. `PianoDmaBegin`：ToDevice clean；FromDevice / Bidirectional clean-invalidate，然后 barrier。
5. 硬件提交及门铃完成判断由调用者负责；缓冲区处于 Active 时不能释放。
6. `PianoDmaComplete`：确认硬件停止后，对设备写入方向 invalidate。无法确认停止则保留映射、隔离 RAM、记录 fault。
7. `PianoDmaUnmap / PianoDmaFree`：清除自有 PTE、clean 页表、TLB sync 后回收。同步或解除失败不复用地址和 RAM。

分配 / 映射 / 提交 / 完成日志包含设备、SID、PA、IOVA、有效长度、保留页长度、方向、对齐、缓存操作及状态。UFS 数据提交另打印实际 transfer length，以区分 32 字节容量响应和其 4 KiB 分配页。

32 / 64 位上限、缓存方向、跨页表边界、只读 PTE、越界、未映射、失败回滚、未停止 DMA 和解除失败均有实际源码的主机测试。64 位 IOVA 当前只有主机验证，不能声称 USB / GPI 设备的 64 位 DMA 已实测。

## UFS 顺序与电源前置条件

1. 保持原 HCE / PHY，确认 MEM_CFG=0（当前为单门铃模式）。
2. DME GET 读取 TX FSM；仅状态为 Hibern8 时执行 Hibern8 exit。
3. 建立自有 SMMU；分配并映射 TRL / UCD。
4. NOP OUT → NOP IN，检查 OCS、响应类型和 tag。
5. QUERY READ DEVICE DESCRIPTOR：先读 2 字节，再按 bLength 读完整内容。
6. 如果 Descriptor 返回 FF，只读 bCurrentPowerMode。第 48 次真实读到 `0x33`（PowerDown）。
7. 仅确认 Sleep / PowerDown 时，发送设备 WLUN D0 的 START STOP UNIT Active：无数据、无 PRDT、无 DATA OUT；随后只读确认 `0x11`（Active）并重试 Descriptor。此步骤恢复读取所需电源状态，不写用户块。
8. Descriptor 成功后才分配 DATA IN 缓冲区，REPORT LUNS → 每个普通 LUN 的 READ CAPACITY(16)。
9. 所有容量成功后，READ(10) LBA0 → LBA1。GPT 头须通过 signature、容量 / 范围及 header CRC；随后只读完整条目数组（当前诊断上限 64 KiB），核对 array CRC 和有效条目的 LBA 范围。

查询、SCSI、sense、残余长度和 OCS 均分别检查；任一步失败不进入下一阶段。门铃超时后停止列表 / 清除请求，无法确认停止则保留 buffer 和页表并冷重启。

## 异常与恢复

同步 / SError 回调把 PC、ESR、FAR、SPSR 写入保留 RAM；已验证的 SMMU 回调补充 global fault、FSR、FAR、FSYNR。递归异常跳过二次探测直接重启。正常实验有定时冷重启，Android 恢复后通过 ADB 回收日志。

第 34 / 37 次分别验证未定义指令和真实 CPU translation fault 的恢复，第 44 次验证未完成 DMA 的内存保留和自动重启。普通 ASSERT 或总线完全卡死不保证触发异常回调。

回收脚本等待对应 test 的成功 fastboot RAM 启动记录，避免把重启前的旧 Android ramoops 当成本轮结果。所有启动固定指定 piano 序列号 `da9cb876`；只使用 fastboot boot。

## 实机证据

| 测试 | 实际结果 |
| --- | --- |
| 43 | 自有 domain / 非 identity IOVA 的软件翻译、map / unmap / close 成功；尚无设备 DMA |
| 44 | NOP 门铃未完成；隔离 RAM、自动重启；26 个启动分区一致 |
| 46 / 47 | Hibern8 exit 后真实 NOP DMA 成功；Descriptor FF，未进入 LUN / 块读取 |
| 48 | 2 字节 Descriptor 仍 FF；只读 bCurrentPowerMode=33，确认设备 PowerDown；26 分区一致 |
| 49 | Active 恢复、完整 89 字节 Device Descriptor、REPORT LUNS（6 个）、全部 READ CAPACITY(16) 成功；26 分区一致 |
| 50 / 52 | 6 份 GPT 头 / 数组 CRC 与 Android 对照一致，26 分区匹配；但部分旧式 RAM 调试字段损坏，不能计为完整日志稳定性验收 |
| 53 | 独立 noncacheable ramoops 后仍捕获 3 条 CRC 不匹配；日志原因未解决，26 分区匹配 |
| 54 / 55 | 同一镜像两轮通过：每轮 37 次命令、21 个只读元数据块、220 条完整 CRC 记录；分别拒绝 6 / 7 份损坏副本并从镜像恢复；Android 容量 / GPT 对照、26 分区哈希和自动返回成功 |

证据位置：`private/analysis/ramlog-test-N/uefi.txt`、`partition-verification-test-N.json`；每次镜像归档于 `artifacts/tests/stage0-test-N/`。

主机检查：`bash tools/test_dma_foundation.sh`，执行 8 组 ASan / UBSan 检查。构建及封装沿用 `build_integrity.py`，失败、旧构建或产物不匹配时拒绝 RAM 启动。

## 写入约束

当前诊断没有块写入路径。真实读取和此 milestone 完成前，不进行实机 UFS 写入。以后首写必须明确专用测试区域，执行备份原块 → 测试写入 → FUA / 同步 → 读回校验 → 恢复原块 → 再次校验；不得用现有系统或用户数据块充当测试空间。

USB 与触摸复用本接口，但各自还需证明设备身份、流 ID、上下文、供电、门铃和完成机制；不能把 UFS 成功等同于 USB / GPI 已成功。USB 下一阶段先验证 EP0 枚举，不扩展 fastboot 命令。

RAM 日志完整性：每条统一 DMA 记录现在带全局序号、命令名、CRC32 和独立镜像；回收工具只选择校验正确的副本（每条记录包含命令名；命令白名单也从 CRC 保护的记录验证），保留原始捕获，任一序号两份都无效或校验正确的内容冲突即拒绝。当前校验工具针对本只读 profile 的 220 条生命周期记录和 37 次命令，不是任意设备的通用数量约束。

日志区映射：将 XBL 的 `Display_Demura` 预留区首 4 MiB（Linux ramoops）单独设置为 noncacheable，余下区间保持 write-through，完整预留范围不变，DMA heap / framebuffer / SMMU 映射没有改变。第 53 次证明单改缓存属性不足以消除损坏，因此保留 CRC / 镜像保护，不声称原因已解决。

当前验证后端为单一 owner、同步轮询 UFS 队列，映射 / 提交 / 完成 / 释放串行执行。未来 USB / GPI 异步驱动需要在后端加入自己的队列同步和 ownership 管理，不能直接并发调用当前 UFS 上下文。

最终产物：`artifacts/dma-milestone/piano-ufs-readonly-gpt-verified.img`，60 秒自动恢复的 RAM 诊断版本。清单 `manifest.json` 记录 54 / 55 次结果、构建 ID、镜像 / FD SHA-256 和限制。55 次 Android 块设备字母的探测顺序发生变化，对照工具按 SCSI LUN 动态查找设备，没有假设固定 sda–sdf 对应关系。
