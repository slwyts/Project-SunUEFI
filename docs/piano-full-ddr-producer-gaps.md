# Current full-DDR producer gaps and concrete wiring

The next implementation must produce two connected contracts: a cold descriptor/
owner contract before the first MemoryPeim, then a live EFI allocation/CPU-source
contract used by the Linux session. Current Core memory validators remain
NOT_READY. A parsed RAM402 table, a composed descriptor table or a host-sized
Linux bundle does not satisfy either contract. No mapping, allocation or device
operation was performed for this review.

## Actual objects and producer entry points

| Object/evidence | Current producer and use | Missing authority/lease evidence |
| --- | --- | --- |
| ABL handoff | `uefi/handoff/bootshim/BootShim.S:15-19` writes magic, x0 DTB and entry EL at A7FFF000 | No original shim/FD-copy-source span, current stack, payload-owner identity or boot epoch is recorded |
| Product FD/current SEC footprint | BootShim copies FD to its compiled FD_BASE; prepared `pianoProductPkg/Sec/Sec.c:94` builds the compiled stack HOB; MemoryMapLib has FD/vector/page-table/stack/SEC/scheduler/FV spans | Need one cold observation tying actual executing PC/SP, compiled span values and the same handoff/boot instance together; do not infer unrecorded old loader extents |
| Factory DTB and combined ABL initrd | `PianoProductPayload.c:59-70` checks the handoff and bounded DTB/chosen initrd against named native map rows, then `106-129` provides a pinned SimpleInit loan | This is a DXE bounded SimpleInit view. Its release retains the parent Kernel reservation; it does not free an ABL initrd or give cold high-DDR ownership. The factory DTB is not automatically the final Linux c2cb DTB |
| Cold SMEM/current/preloaded | `PianoEarlyMemory.c:126-143` observes the cold CPU state and complete two-read RAM402/SIII data; rev2 diagnostic HOB stores the observations | Test96 v3 parsing is successful. Commit58884bc also preserves empty native current records without false overlap; the actual12-current/11-positive capture passes the pinned native identity/ABI fixture. DataValid remains coherent observation, OwnershipVerified remains FALSE, and empty records are not free banks |
| Final Linux reservations | Final complete DTB c2cb041e2b286713c225af7bf0e3a5f7eec8926db168149984823c25ad3f38c4, 1,209,191 bytes | Bind its exact fixed/no-map and dynamic constraints to the cold authority. A new GPIO/driver graph or RAM bank count is not a reservation placement |
| Live EFI/GCD/cache | Real BS GetMemoryMap plus DXE GetMemorySpaceMap/Descriptor; `PianoGuardedRead.c:55-72,100-112` demonstrates current CPU registers, identity AT/PAR and GCD attribute verification for a bounded reader | No full-DDR live validator exists. GCD/cache bits alone do not prove allocation ownership or every live PTE. The existing guarded reader's budgets/window must not be enlarged implicitly |
| File snapshots | `PianoBootFileSource.c:126-128` performs actual AllocatePool(EfiLoaderData), then ValidateBuffer before its first write; Take/Borrow retains the actual source objects | Root needs a producer-lifetime registry for its exact FileSource objects and allocated spans, plus owner/loan transitions. A size-only callback cannot authorize the writes |
| Loaded Linux and initrd copy | LinuxSession loads and records actual LoadedImage identity, then NativeLateArm; Linux's stub independently allocates initrd pages before LoadFile2 copies it | Need a separate typed destination allocation observation bound to the armed/running child and live EFI map. The current LoadFile2 code validates the immutable source and aliases, but does not independently register/validate the destination allocation |

The actual cold insertion point is `pianoProductPkg/Sec/Sec.c:64-88`:
observe cold state → obtain descriptors/authority → retain the existing low
PHIT constructor at81 → publish frozen HOB → call MemoryPeim once at88.
The current source only observes then uses the native map. Calling MemoryPeim
again in DXE or retagging already-live heaps/page tables is not this flow.

The minimum next cold object collector can record, without high mapping or
permission: the fixed handoff record twice, actual PC/SP/EL/SCTLR and compiled
FD/vector/stack/MMU/HOB spans, a protected bounded factory-DTB copy and its
chosen initrd range, and the coherent RAM402 current/preloaded snapshot under
one producer-lifetime epoch. Its read adapter must explicitly permit only the
already-established handoff/native object spans; the existing SMEM reader
permits only its fixed2MiB window and two cookie words. Do not use its cookie
or a DT pointer to authorize an arbitrary new read range.

## Typed composition is still occupied, not free memory

`PianoPlatformMemoryContract.h:8-12` accepts the real native descriptors,
inventory, FDT, explicit KnownOwners(Base,Bytes,DynamicNode) and a CPU arena.
Commit58884bc preserves zero-current observations and skips them as range
candidates at `PianoPlatformMemoryContract.c:82`. The real49-row product table
now composes unchanged: `MmioAlias` at13-19 permits only containing AddDev/
MMAP_IO/EfiMemoryMappedIO/DEVICE rows with identical resource attributes. It
retains USB PHY/PERIPH_SS aliases without accepting mismatched cache, role or
partial overlaps. Positive current ranges, actual owner/preloaded intervals
and the corrected BD980000..D4E23000 low heap remain checked. The actual
product-table/final-DTB/raw-current fixture and inventory tests pass; the
original zero/alias structural blockers are resolved.

Every newly constructed row still uses **EfiLoaderData, WB-XP** at47-48,
including `Piano_CPU_Arena`. Compose still returns ReadyForMemoryPeim=FALSE.
AuthorizeCold re-composes the same inputs and compares rows plus immutable
Fixed/Unplaced/Future counters before and after the real external cold authority.
AcquireForMemoryPeim exposes the table only after exact authorization. The
callback remains unbound; setting it to success would erase the outstanding
cold owner, cache and legal allocation problem.

An occupied arena cannot supply AllocatePool/AllocatePages **while it remains
LoaderData**. It does not have to be Conventional in the initial cold table.
The actual Mu path supports a shorter two-phase design: cold Compose maps and
publishes a genuinely dedicated unused CPU arena as SystemMemory/LoaderData,
WB-XP; a later trusted owner transfers only that arena to the standard allocator
with `gBS->FreePages`, followed by live map verification. This is a normal
allocation-ledger transition, not direct descriptor editing, a second MemoryPeim
or another MMU map. The cold epoch/current-owner/reservation/mapping proofs still
have to exist; changing an authority boolean does not provide them.

The actual Silicium `MemoryInitPei.c:89-102` emits a resource HOB plus a memory
allocation HOB for SystemMemory rows. Mu `Core/Dxe/Gcd/Gcd.c:2767-2789` consumes
that allocation HOB and calls CoreAddMemoryDescriptor using its declared type.
`Core/Dxe/Mem/Page.c:1454-1534` frees an existing non-Conventional ledger range;
CoreConvertPagesEx at644/693-727 permits the transition to Conventional and
does not require an AllocatePages-only cookie. Therefore a cold HOB-created
LoaderData allocation can be transferred when its real producer owns it. The
standard service does not check the caller's arena provenance: the typed cold
ownership record and release gate must do that.

`Page.c:1556-1580` adds an important Mu-specific check: if the memory-attribute
protocol reports NO_MAPPING, RO or RP, CoreFreePages can return SUCCESS **while
leaking the pages**. Success alone is insufficient. Before release verify actual
RW/non-RP normal WB mapping for the whole arena; after release require actual
GetMemoryMap coverage to be Conventional under the expected protection policy.
GetMemoryMap cache bits can be capability-derived, so current GCD/PTE/CPU
evidence remains separate. A descriptor can merge with neighboring same-type
regions: verify exact requested coverage, not exact descriptor-base equality.

The existing composed rows use generic allocation HOBs, not a named producer
lease. Add a cold GUID owner record for the exact `Piano_CPU_Arena` base/pages,
boot epoch and immutable contract/owner-set identity. The late transfer must
match that record and prove it has no allocations, loans, DMA mappings, image,
stack, page tables, ABL payload or current/preloaded firmware user. Do not free
`Piano_DDR_Occupied`, CMA, native fixed rows or other LoaderData allocations just
because their type is reclaimable. Full DDR description and arena release are
distinct states; only the exclusive arena needs allocator publication now.

The final DTB has14 dynamic reserved-memory requests, now represented explicitly
by `FutureLinuxDynamicConstraints=14` in the tested58884bc composition. They are
future Linux memblock requests after EBS, not fabricated current UEFI owners or
fully occupied alloc-ranges. Twelve are restricted below4GiB; debug_kinfo and
dump_mem permit high placements. The complete DT requests stay immutable and
must reach Linux, where real source/copy reservations protect those pages.
Any earlier firmware/UEFI owner is still independently supplied through
KnownOwners and validated by the cold authority. Future requests alone must
not become a current UnresolvedReservations count or an allocation permission;
actual unknown current owners remain a separate unresolved contract.

## Concrete late arena transfer and failure handling

The smallest implementation is one resident typed arena-transfer participant,
bound to the real cold HOB and current owner ledger. Its release operation runs
at APP after native Foundation/backend startup and their required clock/rail
acquisitions, before the first large FileSource or fastboot CPU download
allocation. It performs no new native StartImage. UFS and USB remain active:
their DMA targets are separately verified low allocations, and the arena is an
unused CPU allocation rather than their current buffer. A service slice can run
between transfer chunks; there is no need to retire either controller merely to
publish CPU staging pages.

The participant needs an actual state transition and report, for example cold
owned → transfer-attempted → allocator-owned, with BootEpoch, arena base/pages,
contract digest, before/after map keys, FreeStatus and exact published intervals.
Before its first FreePages call it must:

1. Match the immutable cold record to this boot and the live CPU/MMU generation,
   eligible bank, final DT reservations and actual current owners. Check a
   complete LoaderData ledger covering the exact arena, without a mixed type,
   gap, RP/RO/runtime owner or source/loan/DMA overlap. The existing full-region
   protection/map evidence may establish the mapping; three endpoint AT samples
   alone cannot prove every page of a multi-GiB arena.
2. Claim the participant's one release attempt, prevent recursive release and
   record the original descriptor/protection/map identities in resident state.
   A current EFI type is not a producer-ownership proof.
3. Call standard FreePages only for the exact owned page span, then immediately
   recollect the real EFI map. Accept publication only when all returned pages
   are actually Conventional and live GCD/cache/protection state matches the
   normal allocator policy. No subsequent raw write uses the old arena lease.
4. Let unchanged FileSource AllocatePool and stub AllocatePages allocate normally.
   Register FileSource's actual returned buffer/producer/owner/loan spans and
   validate them before CPU writes. The kernel's independently allocated initrd
   destination needs its separate armed-session allocation validation. The same
   memory provider feeds source loading and NativeLate pre/post-retirement checks.

Preflight refusal makes no allocator change and can leave UI/fastboot usable.
After a service call, uncertain/error/warning/EBS or a partially changed map
cannot be retried as if the arena were still private. CoreConvertPagesEx
`Page.c:677-728,760-845` walks and mutates descriptors incrementally; a request
crossing an unvalidated later gap/type can fail after earlier pages changed.
Preflight a single uniformly typed covering range or explicitly track each
validated piece. Record every actually published interval, retain uncertainty
and do not roll it back with an arbitrary FreePages or descriptor rewrite.
If a failed OS attempt returns before EBS, dispose only its registered source/
destination allocations. The arena remains allocator-owned after a successful
transfer and must not be released a second time.

There is a concrete servicing concern in the current product build, not a reason
to forbid late release: DxeCore AutoGen.c:212 has PcdDebugPropertyMask=2F, whose
DebugLib CLEAR_MEMORY bit08 is enabled. `Page.c:827-838` can clear the whole freed
range while holding the memory lock. A giant FreePages call therefore is not a
zero-load/instant metadata update. Use measured bounded transfer pieces with
APP pump between them and a persistent published-piece ledger, or choose a cold
product build with only that debug-clear behavior disabled and verify its real
setting/protection policy before one full-range call. This review does not change
the PCD. Freed-memory guard policy must also be accounted for: Page.c:818-824
can withhold a Conventional descriptor when that policy is enabled. A SUCCESS
with no real public pages cannot authorize source allocation.

## Actual address limits and native allocation behavior

The unified DMA layer provides a real low-address guarantee:
`PianoDma.c:84-97` uses AllocateMaxAddress capped by the named low DXE_Heap and,
for32-bit devices, MAX_UINT32, then validates the entire allocated span. USB
service endpoint/ring buffers call it with32 bits at `PianoDwc3Device.c:898`.
CPU response frames use AllocatePool at301 and are copied into the bounded DMA
bounce buffer at393. Publishing high CPU arena pages does not move those DMA
allocations or make the device consume a high CPU staging pointer directly.

Mu Pool.c:424-431 backs a large pool with pages;
Page.c:2355-2374 uses FindFreePages with AArch64 MAX_ALLOC_ADDRESS=0000FFFFFFFFFFFF.
FindFreePages at1085-1148 first tries the type's preferred bin and the default
bin, then falls back to any eligible address. Existing small native pools do not
automatically move high immediately, but there is no permanent low-address
promise once a high Conventional region is public. Native Clock/NPA/VCS paths
audited for display use 64-bit pointer loads; no specific32-bit pointer truncation
has been established there. The current diagnostic readers' low-heap envelope
is an admission boundary, not proof that all vendor CPU allocations must be32-bit.
Do not invent a global native32-bit prohibition. Freeze new native driver starts
at the late publication boundary and inspect any actually relevant remaining
allocation/DMA path rather than assuming it is unsafe. Keep all device DMA
allocation limits explicit; native pointer/DMA assumptions still unexamined must
remain identified gaps, not a blanket assertion that FreePages is impossible.

The unchanged stub obtains the second initrd through standard AllocateMaxAddress
with the real ARM64 image-relative limit. The release interval must accommodate
that limit, required contiguous blocks, loaded PE placement and the documented
peak below. Publishing only enough for the immutable source still leaves the
stub unable to allocate its second copy. Root's current memory callbacks remain
NOT_READY until the actual cold/live/arena/allocation participants are bound.

## Two large allocations are live at once

The final shared bootstrap is904,729,600 bytes, SHA256
cf9ae73af00cd96d61354dc429ee48936d8abecf283a0c8639368425c3b05eaf.
Next Image+DTB+immutable initrd source totals948,908,391 bytes; Stable totals
947,851,623 bytes. Root archive page budget is2,642,870,272 bytes inside a4GiB
RAM tmpfs ceiling, which is separate from the UEFI source allocation budget.

The actual stub path `drivers/firmware/efi/libstub/efi-stub-helper.c:571-581`
queries LoadFile2, then allocates a second904,729,600-byte initrd destination,
then copies it. `libstub/mem.c:95-98` uses AllocateMaxAddress,
EFI_LOADER_DATA and220,882 pages. ARM64's actual initrd maximum is tied to the
loaded image's1GiB-aligned base and the minimum linear-map window in
`arch/arm64/include/asm/efi.h:83-90`, with a48-bit allocation limit. The selected
free interval must satisfy that real limit, not a generic UINT64 maximum.

**Next sources plus copied initrd remain live simultaneously:
1,853,637,991 bytes (about1.726GiB), before loaded PE/FDT/options/maps/allocator
overhead.** The actual PE SizeOfImage is43,778,048 bytes for Next and42,729,472
for Stable, aligned to64KiB. Standard LoadImage needs its own image allocation.
The large FileSource initrd Pool also needs allocator metadata/granularity:
Mu `Core/Dxe/Mem/Pool.c:424-431` allocates contiguous page backing for a large
pool. Therefore both initrd allocations need separate eligible contiguous
ranges of at least904,729,600 bytes, with source-pool overhead, plus the other
objects. Existing roughly419MiB low heaps cannot provide either large block;
summing scattered small holes does not satisfy either allocator.

## Live validator and destination lease wiring

After a real cold table has been admitted and the standard DXE map exists,
Root can bind the existing `PIANO_CPU_INPUT_ENV` callbacks at its product OS
controller. CheckMemory must sample the actual map, cold epoch, all known
owners/reservations and current cache state. ValidateMemory must compare that
report to current producer state; `PianoCpuInput.c:7-10,25-34` rejects missing
proof or missing callback for the large source. ValidateBuffer at38-51 must
accept only Root's registered producer, exact owner transition and verified
span, never arbitrary data with matching size. Reuse preallocated map buffers
for repeated APP observations; preserve EBS/retained fences and do not require
the UI runtime to remain alive after retirement.

A further narrow change is required at LinuxSession LoadFile2 before its copy:
`PianoLinuxEfiSession.c:39-41` currently validates only the source view and then
runs CPU Copy. Root needs an actual destination observation identifying the
session and armed/running LoadedImage, checking the full live LoaderData page
allocation, WB mapping and exclusion from all immutable sources/runtime/
reserved owners. Bind this to a typed destination lease, then retain that
allocation through the native late transition. An EFI descriptor type or GCD
ImageHandle alone is not an exact pool/source lease; GCD ownership fields
(`PiDxeCis.h:159-168`) describe GCD resource allocations, not all BS pools.

The same memory provider must feed both LinuxSession's DT-based gates and
PianoLateHandoff's pre/post-retirement checks. `PianoProductCore.c:144-154`
still returns NOT_READY. LateHandoff validates the real child identity at
87-97, checks epoch/DRAM before retirement at56-59 and after it at64-68, and
retains on any uncertain transition. These checks must remain in place while
adding the missing producers. Host artifact validation, raw RAM boot and parsed
SMEM do not override them.
