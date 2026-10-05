# Actual stage1 context readback

The independent Linux canonical commit
`d42158782b81c4aaa47c8643f1400a471785370b` builds on the preserved-route and
identity-readback commits `1fd91b90a720` and `74c517825a95`. It adds the actual
admin-only `piano_dma_context` sysfs getter for attached ARM64 stage1 DMA or
unmanaged domains. It does not force GPU, GMU, Iris, CAMSS or DPU into identity
domains and does not write a hardware-ready file.

The getter verifies the real owning domain, initialized page-table operations,
context map, bank-to-private-config identity, kernel-installed SMR/S2CR route,
CBAR, CBA2R/VA64/VMID16, SCTLR interpretation/endian bits, TTBR0/TTBR1, TCR/TCR2,
MAIR and fault state. Enabled table walks must have real physical roots. Private
software and hardware snapshots are compared twice; visible configuration drift
returns ESTALE. Valid QCOM HUPCF/CFCFG policy differences remain allowed.

An independent source review found and fixed a real removal deadlock: IOMMU group
removal holds its mutex while release_device drains active sysfs callbacks. A
getter waiting for that mutex would prevent its own drain. The new small core
trylock iterator returns EAGAIN when busy, and otherwise keeps the group locked
and referenced through the callback. Domain attach/detach/free and cfg lifetime
are therefore protected without blocking the remove path. The original blocking
iterator retains its behavior. Context fields are root-readable diagnostics.

GPU page-table changes do not use the getter's private init/stream locks. The
double snapshots reject observed changes; the result is configuration evidence
at the read time, never a continuous DMA lease or successful device transfer.
No Root memory publication or CPU access authority follows from this interface.

`tools/check_piano_kernel_contexts.py` validates the exact selected consumer,
actual DT IOMMU provider and SID/mask pairs. It checks the strict kernel ABI and
fresh repeated reads. MDSS has separate prebind identity and postbind stage1
scopes. A live inherited M1 display context may fail the prebind observation;
that diagnostic does not block normal kernel takeover. Mandatory postbind
validation requires real MDSS, GPU and GMU stage1 contexts. Camera/video switch
their singleton IOMMU groups through the normal Linux API before validation.

Actual ARM64 IOMMU core and ARM-SMMU objects compiled successfully. The shared
register comparator executes under ASAN/UBSAN; four host tests cover the strict
ABI, multi-stream state, table/fault/format failures, masked identity and actual
sysfs/DT relationship checks. The kernel source review fixed VA64 and endian
checks in addition to the removal issue. Checkpatch has no remaining errors or
warnings after documenting its OPEN_BRACE false positive for for_each-named
function declarators, which follow the existing kernel brace style.

This is not a device DMA result. The complete kernel and matching external
modules/rootfs must be rebuilt at this exact commit before testing. Previously
sealed kernel and root archives remain historical inputs, not substitute proof
for the changed ABI. ADSP/audio and PCIe still need their actual consumer-domain
binding and readback integration.
