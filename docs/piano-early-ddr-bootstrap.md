# Early DDR bootstrap evidence and remaining binding

The first platform memory query runs in SEC `InitializeMemory`, before
MemoryPeim creates resource/allocation HOBs and configures the MMU. The native
Env RAM protocol is produced in DXE. Its later inventory cannot authorize
re-running the first memory initialization or changing a live DXE map.

The current BootShim record at `0xA7FFF000` preserves its magic, original x0/DTB
at +8 and CurrentEL at +16. It does not preserve SMEM descriptors, RAM item402
or preloaded-image data. The handoff DT is therefore available as a potential
early source, but it is not a complete live ownership ledger.

The native product MemoryMapLib, pinned Linux `sm8750.dtsi` and Android's
`81d00000.smem_region/of_node/reg` agree on the fixed SMEM window
`0x81D00000..0x81F00000` (2MiB). Android has no confirmed safe item402 export;
no physical-memory scan or SMEM payload read was performed by this research.

The captured EnvDxeEnhanced path at RVA `0xBF00` reads the two 32-bit TCSR
cookies at `0x1FD4000` and `0x1FD4004`, forms a descriptor pointer and checks
`SIII` (`0x49494953`). Its descriptor uses size32 at +4, base64 at +8,
itemCount16 at +16, TLVCount16 at +18 and TLVs at +20, including tag `0x4853`.
The actual cookie values, pointer bounds and descriptor contents remain unknown.
They must not be converted into arbitrary address-read permission.

The native RAM path at RVA `0x8638 -> 0xA868` requests SMEM item402 (`0x192`).
The actual payload parser must validate both magic words
`0x9DA5E0A8`/`0xAF9EC4E2`, version32 at +8, count32 at +0x10 and entries at +0x18.

| Supported observed format | Entry stride | Relevant fields |
| --- | ---: | --- |
| v1 | 64 | Base64 +0x10, Size64 +0x18, Category32 +0x24, RawType32 +0x2C |
| v2 | 72 | Same fields, AvailableLength64 +0x40 |

Category14 denotes SDRAM; type1 is the available-bank path. The native preloaded
path uses types5..8 for v1 and5..9 for v2. No actual live payload version has been
obtained, so unsupported versions must remain unsupported. The native
1.5GiB fallback `0x80000000..0xE0000000` is not full-DDR evidence.

The [official Linux SMEM implementation](https://github.com/torvalds/linux/blob/v6.16/drivers/soc/qcom/smem.c)
defines the distinct major11 global TOC and major12 partitioned structures.
The SMEM version word is at +0x5C. For major11, item402's TOC entry is at
`0x81D019F0`; allocated, offset, size and auxiliary-region fields require bounds
checks. Major12 instead requires the final-page `$TOC` at `0x81EFF000`, bounded
partition entries, `$PRT` headers and the actual item headers. Reading major12
through the major11 item offset would be incorrect.

The readonly collector has fixed-window limits, strict item/version/count parsing and repeated
snapshot coherence checks. It must not call native initialization, acquire its
remote locks, allocate, modify SMEM, publish a memory map or grant ownership.
An unbound TryRead adapter, unsupported locator or inaccessible cookie remains
an explicit failure. Static producer-lifetime scratch storage is required
rather than placing two complete payload snapshots on the early SEC stack.

Full publication still needs actual early cookie/item402 evidence, all DDR bank
and preloaded records, fixed/dynamic DT reservation accounting, live firmware
owners, cache attributes and a trusted cold-phase binding before MemoryPeim.
Linux ARM64 EFI boot uses the EFI memory map as its RAM authority, as described
in the [official ARM UEFI documentation](https://github.com/torvalds/linux/blob/master/Documentation/arch/arm/uefi.rst).
Neither an Android RAM count nor a successful raw handoff replaces that proof.

## Bound product SEC observation

`prepare_product_early_memory.py` stages a product SEC from all seven exact
audited upstream SEC file hashes. Its only changes to the original memory
sequence are:

1. `PianoEarlyMemoryObserveCold()` at the start of `InitializeMemory`, before
   either heap lookup and before the first `HobConstructor`.
2. `PianoEarlyMemoryPublishHob()` after the genuine `PrePeiSetHobList`, before
   the original, sole `MemoryPeim` call.

The FDF contains the product SEC instead of the original SEC. No lazy getter
hook is used: DEBUG `PrintFirmwareVersion` already calls `GetMemoryMap` before
`InitializeMemory`, so a getter's first use is not the correct phase boundary.
All native memory descriptors and the existing low-heap correction are kept.
The report never sets `MemoryOwnershipGranted` or `HighDdrPublished`.

The SEC reader requires EL1h (`CurrentEL=4`, `SPSel=1`), MMU and D-cache off,
the current aligned SEC VBAR, and a consistent architectural counter. It has
a 100 ms / 65,536-word total budget and can read only the fixed SMEM window or
the two exact 32-bit cookies. Each word uses the actual AArch64 `LDR` wrapper,
which temporarily installs a 2 KiB aligned private vector, masks interrupts,
permits SError to reach a fatal vector, and restores VBAR then DAIF.

Recovery requires the exact load instruction PC, exact expected FAR, same-EL
data-abort EC=0x25, IL=1, WnR=0, FnV=0 and saved EL1h state. A recovered read
keeps a full ELR/ESR/FAR/SPSR/resume record and returns failure without changing
the destination. An unrelated exception, SP0/lower-EL entry or SError logs the
first syndrome and attempts PSCI `SYSTEM_RESET`; a nested fault in the logger
goes directly to reset using the producer-owned state pointer. If PSCI returns,
the reader retains its guard and stops. Automatic return to Android is an
attempted recovery path, not an untested hardware guarantee.

The static producer scratch buffers avoid a large SEC stack allocation. The
versioned report HOB is GUID `495ec035-44a5-4f17-8750-5049414e4f31`; it contains
the frozen success or failure report, serialized length and CRC32. CRC detects
record corruption and grants no ownership. The original native low map is
used even when collection fails; no high DDR mapping or fallback-bank invention
occurs.

`PianoProductSmem` captures this HOB exactly once before its separate late DXE
guard session. It checks GUID/header/length/version/CRC, duplicate GUIDs,
coherence flags, CPU phase, counts/ranges/raw types and recovered syndrome.
It keeps a private immutable snapshot. Existing product `BeforeRamlog` replay
prints `PIANO_PRODUCT_EARLY_RAM_*` from that snapshot alongside the separate
`PIANO_PRODUCT_SMEM_*` DXE observation; it performs no HOB re-read, native call,
SMEM read or device operation when fastboot freezes the log. Missing or corrupt
early reports remain explicit failures and do not change the low map.

Host verification executes the actual modified SEC `InitializeMemory`, real
Mu HOB builders and real `MemoryPeim` source with injected CPU/load boundaries:
42 cold observation cases, successful and failed discovery HOBs, one PHIT,
one MMU setup, no high-DDR resource or allocation. The real ARM64 COFF objects
are compiled and disassembled; all 16 vector slots, only EL1h sync recovery,
syndrome checks, active-state relocations and VBAR-before-DAIF restoration are
checked. Product consumer tests retain the existing 13 guard scenarios and add
19 malformed/valid/failure HOB cases, including replay after source corruption.
These prove software binding and input handling, not physical MMIO, live page
tables or full EFI RAM availability. No device boot/MMIO operation was performed.

## Current Android memory crosscheck

Read-only Android `/sys/kernel/debug/memblock/{memory,reserved}` capture on
2026-10-06 agrees exactly with the merged union of the actual 2026-10-05 runtime
DT `/memory` entries after rounding partial starting pages inward. Both fixed
CMA regions are covered by Android's reserved intervals. The runtime DT still
has 14 dynamic reservation constraints. Source hashes and exact comparisons
are recorded in `private/analysis/piano-early-ddr-memblock-crosscheck.json`.

This confirms the board's high physical address ranges and existing low-heap
split. Android's current allocations do not describe firmware owners in a
future cold UEFI boot. Early SMEM/preloaded records and a complete current-owner
authority remain required before the typed contract can publish additional DDR
or the firmware can advertise an actual 1 GiB download buffer.
