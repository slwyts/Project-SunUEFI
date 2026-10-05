# HAL-independent SMMU register prototype

This is an offline, injected-register source prototype. Root preserves the
frozen source separately in bootprofiles/smmu-independent/, outside product
inputs; no product driver or physical register operations are bound.
Run `bash build/smmu-independent-prototype/test.sh` for 12 injected-register
cases, ASan/UBSan and strict AArch64 source compilation. No device was accessed.
The product still uses the native HAL backend.

The source validates exact captured SM8750 ID geometry and master idle registers,
reads all127 routes and83 context banks, rejects target SID aliases and selects
only a bank with no active route reference and SCTLR.M=0. Native CBAR free tags
with M=1 are not accepted. It uses caller-owned existing PianoIoPageTable/DMA
memory and injected cache/register/fence/time operations, with no allocator.
Caller must supply exclusive driver-lifetime state; a platform lock adapter is
not present.

Open configures CBA2R/CBAR, TCR before TTBR, MAIR, then SCTLR with exact readback
before activating the sole owned route. It requires actual64bit FSR FORMAT400
with no fault/reserved bits, never arbitrary FSR normalization. It compares all
routes and the captured bank register fields, including ACTLR, of every peer.
Sync uses ownCB TLBIALL618, TLBSYNC7F0, status7F4 and bounded polling. Close
requires fresh master idle, disables the owned route/context, syncs, then
restores selected baseline fields and verifies the captured peers unchanged.
Unknown writes, peer drift or timeout quarantine the supplied table memory.
No global SMMU reset/control, peer register, master-enable, ACTLR or TBU writes
exist. No table free or device DMA completion is claimed.

Register ordering is checked against [official Linux arm-smmu](https://github.com/torvalds/linux/blob/v6.16/drivers/iommu/arm/arm-smmu/arm-smmu.c)
and the captured HAL audit. [Arm's SMMUv2 specification](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)
defines context TLBIALL scope by VMID and permits over-invalidation. Unchanged
peer registers therefore do not prove unaffected peer TLB caches. Qualcomm
implementation differences are explicit in [official Linux qcom support](https://github.com/torvalds/linux/blob/v6.16/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c).

Not-ready items: safe access to all83 banks (an injected read error already
refuses with zero writes), secure-world ownership, actual candidate eligibility,
ACTLR/TBU behavior, early clock/dependency ordering and real translation/DMA.
No live platform ops are bound. The current Sync requires master idle and does
not support new mappings under a running device. Shared allocator/locking/map
integration and early-NV DXE ordering remain to be implemented and tested.
Exact baseline route restoration also differs from native zero-row detach;
the retired-peer proof consumer must be adapted deliberately during integration.
Do not report early DMA or HAL-free product readiness from these host results.
