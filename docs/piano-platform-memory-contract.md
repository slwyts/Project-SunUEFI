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
data, the native synthetic fallback or bad bounds refuse composition.

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

The row count is bounded by the real platform limit128 (leaving the MMU table
terminator slot), names and spans remain bounded, and no BS/allocator/MMU or
target DDR access occurs. The input fingerprint is a coherence check over all
actual DT/native/inventory/owner bytes, not a cryptographic ownership claim.
Auth re-composes the same table and rejects changed inputs or rows.

Composition always returns `ReadyForMemoryPeim=FALSE`. The public acquire
function refuses to expose a table for MemoryPeim until a real Root/platform
authority approves the current cold phase and every ownership condition; it
can expose the authorized table only once. NULL authority and warnings refuse.
The unresolved dynamic-constraint count is reported to that authority, which
must establish actual placements or prove their consumers are inactive in the
current phase. Passing a hardcoded success callback is only used by the host
fixture and is not a valid hardware implementation.

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

`tests/test_product_low_memory.py` executes three checks:

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
