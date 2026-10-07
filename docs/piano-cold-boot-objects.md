# Cold BootObjects owner inputs: product SEC binding

The new `uefi/handoff/early-memory/PianoColdBootObjects.c/.h` is a real SEC
collector for occupied boot-object inputs. The reviewed `PianoBootObjectsShim.S`
and actual `uefi/handoff/bootshim/BootShim.S` now contain byte-identical extension
source. `prepare_product_early_memory.py` binds the collectors once to product
SEC and stages a separate read-only DXE consumer. This binding has host
verification; its actual cold report remains to be captured from the next
uniquely built image. MMU and resource/free-page publication remain unchanged.

The actual prepared binding is a single Observe before the first HobConstructor
and MemoryPeim, followed by PublishHob after the existing PrePeiSetHobList and
before the sole MemoryPeim. Original SMEM Observe/Publish calls remain in order
with unchanged budgets. The collector uses its separate `PianoColdSecRead256`
short batch LDR/fixup; the original `PianoSecRead32` remains unchanged.
It has a separate admission policy for the fixed
BootHandoff page, the checked original shim header and the checked factory-DTB
span. It does not modify the SMEM adapter, cookie permissions or SMEM budgets.
There is no BS service, target write, cache/MMU operation, resource/allocation
HOB, high-DDR read or high-DDR/free-page publication.

Observe records actual current PC/SP/EL/SCTLR/VBAR/DAIF/SPSel/TTBR0/TTBR1 and
counter/frequency. Current EL1, SPx, MMU and D-cache off are mandatory; PC/SP and
the active2048-byte vector table must lie in the compiled FD/stack/vector
spans. The actual compiled native map supplies FD, stack, vector reservation,
MMU reservation, SEC heap, scheduler heap, FV and BootHandoff occupied spans.
The disabled translator's TTBR values are diagnostics. The compiled MMU row
is a reservation, not a claim that those bytes contain live page tables.

The same assembly ABI retains the three legacy words at0/8/16, and the ARM64
image magic at byte56. The new144-byte handoff layout is:

| Offset | Field |
| --- | --- |
| 0 / 8 / 16 | Legacy magic / x0 DTB / entry CurrentEL |
| 24 | Extension magic314A424F544F4F42 |
| 32 / 36 | Version1 / exact record bytes144 |
| 40 / 48 | Entry counter / frequency |
| 56 / 64 | Original shim base / bytes through Payload |
| 72 / 80 / 88 | FD source / compiled destination / FD bytes |
| 96 / 104 / 112 | Entry SP / VBAR_EL1 / SCTLR_EL1 |
| 120 | Flags: copy loop completed1, EL1 path2, SP4, PC8, counter16, EL1 registers32, copied rather than in-place64 |
| 128 / 132 / 136 | CRC32 / reserved-zero word / actual original Start PC |

EL other than the established EL1 path does not execute the EL1/counter MRS
instructions. Its unknown fields remain zero and the collector cannot accept
it as complete. Counter ties observations to the same handoff instance; it is
neither a cryptographic boot identity nor memory ownership. CRC32 protects the
record's structure with its own4-byte field treated as zero, not source identity.

The collector requires exact revision/size/reserved bits/CRC, stable two reads,
EntryPC inside the shim, FD source equal to ShimBase+ShimBytes, matching compiled
FD/Pcd geometry, and the original64-byte ARM64 header's FD fields/magic. It
rejects a shim or FD source that intersects the144-byte metadata write window,
and rejects nonidentical overlapping FD copy source/destination. In-place is
explicitly supported. CopyCompleted describes the assembly control path; it
does not certify the entire copied FD content. No whole-FD hash or incoming
ABL stack extent is invented. EntrySP is a point observation, while the current
SEC stack has a compiled, checked extent.

The factory-DTB reader is deliberately a bounded `/chosen` metadata extractor.
It validates header/version and nonoverlapping structure/string ranges, the
initial reserve-table bounds, token/node depth, property bounds and initrd
start/end. It extracts the first matching direct-root `/chosen`, matching the
existing libfdt path consumer, and stops when that node closes. Duplicate
initrd-start/end properties within that node still fail. It does not claim
there are no later duplicate chosen nodes. Two independent views clear their
structure/strings caches and compare the exact header plus chosen boundaries.
`DtbHeaderCrc32` covers only the40-byte header. It does **not**
claim full-DTB byte coherence, a full DT validator, all mem_rsvmap entries or
final-Linux reserved-memory ownership. The complete declared DTB and initrd
spans must stay inside the already-known native Kernel/low-heap input windows.
Final-Linux fixed reservations and future Linux memblock constraints remain
the separate full-DDR contract's job.

Scratch and each structure/strings cache are256 bytes; parse temporaries are
small and no multi-MiB BSS/snapshot is added. Cache validity requires a complete
guarded fill. Logical DTB size and the protected aligned read span are separate:
at most3 padding bytes are checked against the native input range, never parsed
or added to the reported object length. The independent budget remains
4MiB+64KiB of attempted aligned word reads and2 seconds. CPU freshness is
checked before/after each bounded batch, and batch assembly checks SCTLR
stability before each word. Reports preserve attempted words, guard batches,
cache fills/hits, recovered faults, elapsed
ticks/usecs and last requested/completed read address. Exhaustion returns
NOT_READY/error with a budget reason; it does not convert partial observations
into coherent inputs. Actual device timing of the optimized path remains to be measured.

Test105's earlier per-word implementation timed out after927 loads in
2,000,765us, with no recovered faults; handoff and FD provenance had passed.
The actual1,110,810-byte capture has chosen at structure offsets61C..E7C and
strings atFDB54. Scanning the unrelated remainder and repeatedly installing
vectors for every name byte was unnecessary for this metadata claim. The
actual fixture now uses36 guard batches,1822 target words,28 cache fills and
698 hits for two views. These are host operation counts, not a tablet ETA.

The independent batch admits only aligned1..64 words and installs its vector
once. One fixed LDR PC updates state.Address before each target word. Precise
recovery requires that ELR, exact current FAR, load-abort EC/IL, WnR=0, CM=0
(ESR bit8), FnV=0 (bit10), and EL1 SPx SPSR5 all match. Fault exits without
advancing/copying the failed word. Partial scratch stays private and cannot
become collector output. Unknown/ownerless/nested exceptions and SError reach
the existing fatal/reset path. Final DSB/ISB completes with SError vector still
owned, then VBAR/DAIF restore in the existing order. SMEM retains its original
single-word implementation. Report version2 adds counts and exact HOB size;
BootShim/handoff remains ABI version1.

PublishHob separately validates the current legal EL1/SPx/MMU-and-Dcache-off
CPU writer phase and actual FD/stack, without reusing the probe time/load limit.
Thus an expired probe budget or incomplete/legacy observation can publish its
frozen failed diagnostic when the current CPU and PHIT are legal. It does not
continue any target read or upgrade the failed Status/Reason. It then validates
the real low-heap PHIT/type/length/free bounds and
space, then creates one GUID HOB. The appended occupied HOB prefix comes from
the actual PHIT/free-bottom values. Failure before creation leaves no empty
typed HOB. The report is frozen with exact layout/CRC and explicit status;
consumer validation rejects bad sizes, reserved/permission bits, corrupt spans
and inconsistent handoff/PC/SP/header/owner relationships. A diagnostic legacy
or incomplete report remains NOT_READY. All `MemoryOwnershipGranted`,
`HighDdrPublished` and `AuthorityReady` fields stay false. An accepted object
record supplies specific occupied spans; it does not complete the platform
owner inventory or admit any new Conventional page.

Run `python -m unittest discover -s tests/unit -p test_cold_boot_objects.py -v`.
The actual C collector is linked against the actual current53-row native map
and real Mu PrePiHobLib, with CPU/LDR boundaries substituted.24 UBSAN fork
cases cover current CPU/span checks, legacy/ABI/source/PC/CRC failures, in-place
copy geometry, stable reads, fault/CPU drift, time exhaustion, actual GUID HOB
publication/tampering, and prechecked invalid/full PHIT without HOB mutation.
Actual AArch64 assembly is built/disassembled; a generic interpreter consumes
those instruction operands to run the real3MiB forward copy and CRC loop for
EL1, non-EL1 and in-place paths. It checks all metadata writes stay inside
`[A7FFF000,A7FFF090)`, the payload offset equals binary length, ARM header56,
exact flags/reserved/CRC and the final branch. This is host instruction
modeling, not physical ARM execution. AArch64 C syntax also passes.

`PianoColdBootObjectsContract.c` contains the shared pure validation used in
SEC tests and DXE; the DXE INF cannot link the SEC collector. The preparation
record hashes each staged source/schema and the real BootShim input, rejects
drift/duplicate INF sources, and retains exactly one MemoryPeim. Root supplies
the product Core call sites; this binding does not modify Core/display code.

`PianoProductBootObjectsReemit(Alive)` captures only the unique exact-length
GUID HOB, rejects duplicates, copies twice and validates frozen CRC/shape and
all permission bits. Its report getter distinguishes a valid failed diagnostic
from invalid/missing HOB data. Subsequent replay uses the owned cache only;
it neither reruns SEC nor rereads handoff, DTB, SMEM or hardware. Initial/failed
capture, each object log and final return check the actual Root CPU lifetime
fence. Captured Status/Reason/elapsed/read/fault fields and every typed object
are reemitted before Fastboot log capture. Additional21 ASAN/UBSAN consumer
cases cover missing/duplicate/truncated/CRC/recomputed-tamper/permission bits,
copy drift, EBS and cache-only replay. The actual collector/HOB test also proves
timeout and legacy failure diagnostics can publish and replay as NOT_READY.

The next step is Root's unified product preparation/build and actual report
capture, then occupied inputs can feed an independently proved cold full-DDR
authority. The current high DDR/Conventional gate remains unchanged and NOT_READY.
