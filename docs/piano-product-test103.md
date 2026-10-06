# Product test103: real display AHB acquire, hold and retirement verified

Product build8026568c-7e6c-4375-962d-6dd601c3e27e, image SHA256
aa47ec1d372b2cb5adaa684f72ebf01241f0936d49fd7a0228b3bd1402a729db,
passed393 host tests and actual firmware build/identity validation. RAM-only
fastboot boot succeeded. No flash, persistent media write or routine partition
hash pass occurred. Physical panel acceptance remains pending the operator's
reply; framebuffer pixels alone are not a panel observation.

The real native Clock lease now succeeds:

| Evidence | Actual value |
| --- | --- |
| Native image / clock ID | CFF05000 / 04010033 |
| Identity / GetID / Enable / counters / cleanup | Success |
| Ordinary total references | 0 to1 |
| Ordinary current-client references | 0 to1 |
| Acquisition matching snapshots | 2 before / 2 after |
| GCC display AHB127004 | 88000002 to88000003 |
| IsEnabled / IsOn | 1 / 0 (allowed HWCG idle state) |
| Held / owned / retained / services lost | 1 / 1 / 0 / 0 |

The actual bounded CPU reader completes1652 short sessions and41844 word reads.
Its last ClientRef object is D0017A18/18 bytes, reading counters atD0017A28;
anchorCFF38AC0 equals native base+GCC node33A68+58. Actual EFI descriptor15 is
BootServicesData/type4, D0013000/C pages, attributes100F, virtual0. Its cache
capabilities contain0xF, confirming why the former exact-WB comparison refused
the map. Actual GCD/PAR checks also passed. The successful path does not use the
new clean-refusal exception: proofNotStarted, accepted0.

Standard fastboot version/oem ramlog/getvar/get_staged commands succeed with the
reference held. The262144-byte log CRC32 is D580EF7C. A complete3200x2136 BMP of
20505654 bytes has CRC32 D2D1EDC9, matching device metadata and the earlier
software menu image. Its successful upload takes7.173 seconds (CLI reports7.170).
DISPCC/DPU target reads remain unstarted; this reference is not a DPU-domain or
complete display-readiness grant.

Standard fastboot reboot then exercises the real owner callback. The retained
log records:

    PIANO_DISPLAY_RELEASE status=Success clean=1 owned=1/0 refs=1/0 retained=0

The manager validated the actual typed report after USB/UFS/input cleanup,
including fresh reference decrement, GCC readback, reader cleanup and lease
event closure, before permitting reset. The existing UFS shutdown also reports
clean1 with stopped queues/IRQ, freed DMA, closed domain and released clocks.
Live Android boot_completed=1 confirms recovery without a manual reset.

This proves one actual native display AHB reference across the product UI and
its exact retirement. It does not establish physical white-screen recovery,
other display power/clock ownership, high DDR or full Linux admission.

Evidence: private/analysis/usb-live-test103/{ramlog.bin,ramlog.txt,manifest.json,
screen.bmp,screen-transfer.json,reboot-command.json}, and the preserved
private/analysis/ramlog-test-103/console.txt. The sealed image and hardware result
are under artifacts/tests/stage0-test-103.
