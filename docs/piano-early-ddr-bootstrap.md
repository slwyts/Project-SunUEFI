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
| v2/v3 | 72 | Same fields, AvailableLength64 +0x40 |

Category14 denotes SDRAM; type1 is the available-bank path. The native preloaded
path uses types5..8 for v1 and5..9 for v2/v3. Test92 observed actual version3;
the remaining real payload contents still need diagnostic capture. Unsupported
future versions remain unsupported. The native
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

## Test92 actual v3 and the exact native parser branch

Test92 produced actual cold evidence: SMEM major12 (`0xC0000`), RAM item402
version3, address`0x81D06AD0`,2328 bytes, CRC32`0x7C271814`, repeated metadata
and payload equal, stable successful cookie`0x81EFF350`,1762 word loads and
zero recovered aborts. Its parser rejection was our former v1/v2-only gate,
not missing physical SMEM access. These observations remain distinct from
DDR allocation/ownership authority.

The byte-identical captured EnvDxeEnhanced SHA
`593d9e766c0070e01d4c6684b7bb8050f32f77ea3ca300c6a110ad3903de17e3`
provides the exact layout: at8b54..8b80 only version1 selects the64-byte path.
Other versions enter8b90;8ba8 sets cursor=payload+0x30,8bc0 reads type at
cursor+0x14 (=entry+0x2c),8bd0 reads category at+0xc (=entry+0x24),8bc4 reads
base at-8 (=entry+0x10),8be8 reads available length at+0x28 (=entry+0x40),
8c6c reads preloaded size atcursor (=entry+0x18), and8ccc advances0x48 bytes.
This proves the72-byte v3 branch independently of2328's arithmetic. The new
parser admits exact versions1/2/3, still rejects4/future versions, and keeps
every existing range/count/category/type/coherence gate. Product's frozen-HOB
validator accepts the same exact v3 ABI. No fallback or ready bit is added.

The next product also logs the coherent saved RAM402 header and full bounded
raw payload in24-byte rows, even when a semantic type/range check refuses it.
No second physical read or RAM scan is used. This permits exact reconstruction
of remaining real bank/preloaded data from retained logs without guessing.

`PianoSmemDescriptorCollect` observes native `SIII` only when the stable cookie
falls entirely inside the existing fixedSMEM window. It parses the actual
20-byte header and bounded native TLV walk, at most64 TLVs/2KiB, compares two
complete snapshots, reports raw64-byte prefix, base/size/items/TLV/host tag,
CRC and fixed-window agreement. Unknown unaligned/oversized encodings remain
explicit failures. Neither its advertised base nor its size grants external
address permission. The SEC INF now includes the real descriptor implementation.
Raw payload/SIII diagnostics are appended to the persistent cold RAM log;
the existing early HOB version/CRC remains unchanged.

Actual-source tests now cover58 RAM parser cases,44 cold SEC cases and12 fixed
descriptor cases; a separate check hashes and disassembles the exact native
v3 branch. Root still needs the next actual payload/descriptor and current-owner
facts before composing/publishing high DDR. BootShim source and memory-map
publication have not changed in this v3 repair.
