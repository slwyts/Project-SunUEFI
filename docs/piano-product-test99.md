# Product test99: CPU framebuffer metadata unchanged, 256 KiB fastboot log

Product build `95627c11-ed7c-4556-b563-80e545bfc024`, image SHA256
`3ff1b7501152fa0135f86d27eef8b92049231ee6ec1044f77422a4d05f3a0241`, passed
380 host methods and actual firmware build/identity checks. RAM-only fastboot
boot succeeded. The operator confirmed the physical screen still blank
white/grey. No flash or persistent media writes occurred.

Stock fastboot version, oem ramlog, size/CRC and get_staged succeeded. The full
262144-byte snapshot has CRC32 EF7BAA56 and uploaded in0.187 seconds. This proves
the enlarged log export on the real product USB service, not just a fixture.
Detailed replay contains all30 framebuffer and all30 MDSS snapshots, including
each actual native pre/post phase and four outer phases. The raw log and
independent complete-field comparison are preserved under
private/analysis/usb-live-test99.

All30 framebuffer groups have two complete coherent rounds and six samples,
with no observed field change. Fixed sampled pages FC800000,FD509000,FE212000
translate identically and without PAR fault. SCTLR30D0198D, TCR480803514,
TTBR0D4E22000, TTBR1zero, MAIRFFBB4400 and DAIF200 stay unchanged. The physical
GOP remains D0897E00, FC800000/1A13000,3200x2136,stride3200,format1.

The GCD descriptor reports Reserved/type1, baseFC800000/2B00000, attributes4
(WT). CPU AT returns attribute44 (Normal NC), already at before-foundation and
through after-usb. The static product row declares WRITE_THROUGH_XN; the pinned
MAIR definitions distinguish44(NC) fromBB(WT). This discrepancy is retained as
evidence. It is not a measured native pre/post change and is not established as
the white-screen cause. Three samples do not prove the entire framebuffer,
all PTEs or an EL2 translation state; whole_range_ready stays0.

The MDSS groups also remain identical within this boot. Raw cross-boot FAR/
FSYNR differences are not a new-fault report. CPU metadata and the measured
SMMU state have not exposed a trigger; DPU fetch/plane, DSI and display clocks
are still unobserved. Their ROM ranges are absent from the current static
product map, so new register loads require a separately verified mapping and
precise bounded whitelist, not an arbitrary late mapping or cache change.

Standard fastboot reboot succeeded in1.206 seconds. The recovered log reports
UFS clean1, stopped queues/IRQ, domain closed, DMA/protocol release and clocks
released, with other-owner/global-clock verification explicitly not claimed.
Live Android sys.boot_completed=1 was checked. No routine partition hash pass
was performed.

This build also contains the tested zero-current/MMIO-alias producer fixes.
Coherent native inventory and structurally valid composition still do not
authorize high DDR, create free Conventional pages or admit full Linux boot.
