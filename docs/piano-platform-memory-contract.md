# Product cold low-heap fix and full-DDR typed contract

The product preparation now fixes two proven conflicts in its actual cold-boot
MemoryMapLib source. It does not retag allocations in a running DXE instance:

| Region | Product table treatment |
|---|---|
| BD930000..BD980000 (320KiB ADSP intersection) | Reserved resource HOB, HobOnlyNoCacheSetting; no PHIT, allocator or CPU mapping |
| BD980000..D4E23000 (372.63671875MiB) | unique named DXE_Heap; original WB attribute and Conventional type |
| D4E23000..D5100000 (2.86328125MiB HWFence) | Reserved resource HOB, HobOnlyNoCacheSetting; no PHIT, allocator or CPU mapping |
| D5100000..D8000000 (47MiB) | separate DXE_Heap_Upper system-memory/Conventional resource, original WB attribute |

`fix_product_low_heap()` in `prepare_product.py` validates the exact fixed
reserved-memory ranges and no-map properties using the existing DT parser,
then replaces one known original heap row. All unrelated native rows and cache
attributes remain unchanged. DBI_Dump remains NoHob and UC, but its misleading
Conventional token is changed to Reserved. The product-only helper is applied
by normal `prepare_product.py`; old diagnostic platform tables stay untouched.
No high DDR region is added by this low-heap fix.

SEC `InitializeMemory` looks up the unique `DXE_Heap` and calls HobConstructor
with that one base/length. It cannot represent a PHIT arena spanning a reserved
hole. Choosing the lower 372MiB segment gives a large contiguous early arena
without covering ADSP/HWFence. Upper47MiB is a separate resource consumed by
the normal DXE GCD/EFI initialization later. `PianoDma.Heap` also matches the
same exact lower name, so its AllocateMaxAddress ceiling excludes the upper
segment; no implicit DMA change is made. The product payload registry accepts
the separately bounded `DXE_Heap_Upper` source region, and never joins rows
across a protected hole.

## Full DDR C builder

`PianoPlatformMemoryContract.c/.h` construct actual
`EFI_MEMORY_REGION_DESCRIPTOR` rows for MemoryPeim, rather than another JSON
planner. Inputs are the real verified RAM inventory, bounded handoff DT, the
already corrected native table, explicit current owner intervals, and an
optional occupied CPU-arena request. Missing inventory, unverified/retained
data, the native synthetic fallback or bad bounds refuse composition. The pinned
native inventory now preserves zero-length current records as empty observations;
only positive current spans contribute to coverage or overlap checks. Its
`DataValid` flag means coherent native records, while `OwnershipVerified` remains
false. No observed native record alone is a physical ownership grant.

The builder reads the actual DT through FdtLib. It intersects DT RAM banks with
the native available bank inventory, protects partial pages, preserves every
native descriptor byte, and excludes fixed firmware/no-map, FDT reserve-map,
preloaded images and supplied current owner ranges. Disabled fixed reservations
are protected; alloc-ranges are never mistaken for fully occupied regions.
Fixed reusable shared-dma-pool regions are represented as occupied LoaderData
with WB-XP only when they are backed by both RAM descriptions. Ordinary newly
described DDR uses the same occupied type. No new Conventional region is
created. Requested CPU arena must have complete uninterrupted eligible
coverage and may not consume CMA or any recorded owner.

The actual product table has two MMIO subrange aliases: Piano_USB2_PHY and
Piano_USB3_PHY lie inside PERIPH_SS. Composition accepts a containment alias
only when both rows are AddDev, MMAP_IO, EfiMemoryMappedIO and DEVICE, with equal
resource attributes. It preserves both original rows and their order byte for
byte. Different cache/type/resource attributes, partial overlap and every
DRAM/allocator-space overlap are rejected; no native row is merged or retagged.

The row count is bounded by the real platform limit128 (leaving the MMU table
terminator slot), names and spans remain bounded, and no BS/allocator/MMU or
target DDR access occurs. The input fingerprint is a coherence check over all
actual DT/native/inventory/owner bytes, not a cryptographic ownership claim.
Auth re-composes the same table and rejects changed inputs or rows.

Composition always returns `ReadyForMemoryPeim=FALSE`. The public acquire
function refuses to expose a table for MemoryPeim until a real Root/platform
authority approves the current cold phase and every ownership condition; it
can expose the authorized table only once. NULL authority and warnings refuse.
The 14 size/alloc-ranges nodes are reported independently as
`FutureLinuxDynamicConstraints`: generic Linux reserved-memory allocates them in
memblock after EBS, rather than requiring invented UEFI addresses. Their complete
constraints stay in the original fingerprinted DT. The legacy
`UnplacedDynamicConstraints` counter does not count these future requests; zero
there does not prove that all current firmware owners are known. Actual supplied
current owner intervals remain excluded regardless of their DynamicNode label.
The real cold authority still has to establish current ownership, kernel phase
and the future-request disposition. All phase counters are compared again during
authorization so a caller cannot alter them while preserving the rows. Passing
a hardcoded success callback is only used by the host fixture and is not a valid
hardware implementation.

## The bootstrap ordering constraint remains real

Native RAM inventory is produced by EnvDxeEnhanced in DXE. The first SEC
MemoryPeim/HobConstructor/MMU initialization precedes that driver. A late DXE
snapshot is therefore not automatically a valid source for the first cold-boot
map, nor can calling MemoryPeim a second time safely replace a live TTBR/GCD.
Root still needs a trusted early handoff/SMEM discovery and current owner
adapter, or a separately verified atomic DXE expansion path. The present module
does not implement or authorize that adapter and cannot turn a successful
inventory getter into DDR ownership.

For a future early MemoryMapLib binding, store the complete contract in static
producer-lifetime storage, call Compose and the genuine cold authority before
HobConstructor/MemoryPeim, and use AcquireForMemoryPeim as the only publication
gate. Do not apply the raw composed rows or enable the 1GiB advertisement merely
because the structural builder succeeds. Current product download capacity
remains64MiB and the high table remains unbound.

## Actual-source verification

`tests/test_product_low_memory.py` executes four checks:

- Original captured and current Android DT reservation semantics yield the
  same product low fix; all unrelated native rows/cache fields are unchanged.
- Verbatim actual SEC InitializeMemory, MemoryMapHelper and PianoDma.Heap run
  with the corrected table; real Mu MemoryPeim/AddHob and PrePiHobLib create the
  expected lower/upper allocation/resource HOBs and no protected CPU descriptor
  or Conventional allocation. The physical arena backing is a host fixture.
- The full-DDR C builder parses the real captured DT through pinned libfdt,
  and actual Mu HOB builders consume its host-authorized table. It checks the
  occupied1GiB resource/allocation, both fixed CMA HOBs, WB-XP MMU inputs,
  source preservation and default-unready/fallback/owner/preloaded/CMA overlap,
  stale input, warning and repeated-publication refusal.

ARM64 strict syntax checks pass. Existing actual ArmMmuLib attribute/address
tests remain the page-attribute conversion check. These results prove source
and HOB/MMU-input behavior, not live PTEs, physical DDR or EFI allocator access.
No firmware build, device operation, kernel/config/pin change or high-DDR
mapping was performed in this work.

The producer compatibility tests additionally use the actual prepared 49-row
product MemoryMapLib, final shared Stable/Next DTB SHA256
`c2cb041e2b286713c225af7bf0e3a5f7eec8926db168149984823c25ad3f38c4`,
and unchanged physical test95 RAM402 bytes. They compose a host-only occupied
2GiB CPU-arena request, preserve all 49 native rows, reproduce identical output,
retain native empty-current observations, and reject MMIO attribute/role/partial
overlap mutations, DRAM aliases, current overflow/overlap and real owner conflicts.
They never call an authority, MemoryPeim, MMU or a high-memory allocator.

`tests/test_ram_partition.py` runs 31 actual inventory/identity/ABI/lifetime
cases against the pinned original Env PE, including the physical current12 view
behind the substituted ARM-call boundary. `tests/test_product_low_memory.py`
runs four methods including the 16-case actual-product compatibility fixture.
Those producer fixes remove structural refusals; they do not make the full Linux
plan ready. Newly composed DDR is still occupied LoaderData, with no new free
Conventional arena. Real cold authority, live map/cache/owner evidence and the
separate large EFI-stub allocation capacity remain future work.
