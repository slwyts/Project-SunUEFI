# Cold BootObjects owner inputs: isolated host candidate

The new `bootprofiles/early-memory/PianoColdBootObjects.c/.h` is a real SEC
collector for occupied boot-object inputs. `PianoBootObjectsShim.S` is its
isolated assembly candidate. Neither is bound to the current product.
The actual `bootprofiles/handoff/BootShim.S`, SEC preparation, Core, display
modules, MMU and memory publication code are unchanged by this work.

The intended binding is a single Observe before the first HobConstructor and
MemoryPeim, followed by PublishHob after the existing PrePeiSetHobList and before
the sole MemoryPeim. The collector calls the existing real `PianoSecRead32`
short LDR/fixup directly. It has a separate admission policy for the fixed
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
start/end. It parses two independent views and compares the exact header plus
chosen boundaries. `DtbHeaderCrc32` covers only the40-byte header. It does **not**
claim full-DTB byte coherence, a full DT validator, all mem_rsvmap entries or
final-Linux reserved-memory ownership. The complete declared DTB and initrd
spans must stay inside the already-known native Kernel/low-heap input windows.
Final-Linux fixed reservations and future Linux memblock constraints remain
the separate full-DDR contract's job.

Scratch is256 bytes and parse temporaries are small; no multi-MiB BSS/snapshot
is added. The independent budget is4MiB+64KiB of attempted aligned word reads
and2 seconds, including repeated header/string/parser access, rather than
counting each DT byte once. Each word performs the actual short guard and CPU
freshness checks. Reports preserve attempted loads, recovered faults, elapsed
ticks/usecs and last requested/completed read address. Exhaustion returns
NOT_READY/error with a budget reason; it does not convert partial observations
into coherent inputs. Actual device timing of this unbound candidate is unknown.

PublishHob first validates the real low-heap PHIT/type/length/free bounds and
space, then creates one GUID HOB. The appended occupied HOB prefix comes from
the actual PHIT/free-bottom values. Failure before creation leaves no empty
typed HOB. The report is frozen with exact layout/CRC and explicit status;
consumer validation rejects bad sizes, reserved/permission bits, corrupt spans
and inconsistent handoff/PC/SP/header/owner relationships. A diagnostic legacy
or incomplete report remains NOT_READY. All `MemoryOwnershipGranted`,
`HighDdrPublished` and `AuthorityReady` fields stay false. An accepted object
record supplies specific occupied spans; it does not complete the platform
owner inventory or admit any new Conventional page.

Run `python -m unittest discover -s tests -p test_cold_boot_objects.py -v`.
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

The next separately reviewed step is to bind this candidate extension and SEC
collector once, capture actual reports on the device, then feed the occupied
inputs into an independently proved cold full-DDR authority. The current high
DDR/Conventional gate remains unchanged and NOT_READY.
