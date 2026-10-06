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
| ABL handoff | `bootprofiles/handoff/BootShim.S:15-19` writes magic, x0 DTB and entry EL at A7FFF000 | No original shim/FD-copy-source span, current stack, payload-owner identity or boot epoch is recorded |
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

An occupied1GiB arena cannot supply the current reader's standard
AllocatePool or the kernel stub's standard AllocatePages. The legal minimal
extension for the unchanged standard loaders is to have the cold authority
identify, prove and publish additional **Conventional** page ranges before
first MMU/HOB construction, excluding every live owner, fixed/no-map/CMA and
phase-relevant dynamic placement. That requires an explicit typed authorized
allocation class in the cold descriptor producer; changing the current occupied
row's type after Compose is not sufficient. An alternative source-only arena
allocator would require a real reader/allocator integration which is currently
absent, and still would not satisfy the stub's independent page allocation.

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
