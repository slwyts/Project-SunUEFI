# Native Clock typed CPU reader

`bootprofiles/uefi-app/PianoDisplayClockRead.c/.h` supplies the real protected
CPU read callback for `PianoDisplayClockLease`. It never calls a Clock method,
reads GCC/MMIO, changes a mapping, writes a native object, allocates DDR, or
grants DMA ownership. `MemoryOwnershipGranted` remains false. The lease's
actual native reference ledger and its independent GCC reader remain separate.

Root initializes persistent zeroed reader and lease objects with:

```c
PIANO_DISPLAY_CLOCK_READ_ENV env = {
  .Context = rootContext, .Services = gBS, .DxeServices = gDS,
  .BootServicesAlive = rootAlive, .Lease = &lease
};
status = PianoDisplayClockReadInitialize(&reader, &env);
// Lease ENV.Context = &reader; ENV.ReadCpu = PianoDisplayClockReadCpu.
// Its Alive callback and separate ReadGcc callback use that same real context.
```

Initialization retrieves the actual Clock FV PE and verifies all 278528 bytes
against SHA256
`f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e`.
The source is
`upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi`,
file GUID `4db5dea6-5302-4d1a-8a82-677a683b0d29`. During the lease's existing
Source reads the adapter independently compares each sequential live text
chunk to its own relocation-normalized FV copy. Only captured DIR64 text
relocations are normalized. All text `[base+1000,base+28000)` must match before
any data/typed graph read is admitted; no caller-provided verified boolean
substitutes for this comparison. The adapter frees its FV copy at completion
with exact successful FreePool. A failed or uncertain free retains it.

Every read checks the fresh actual LoadedImage handle/interface, base,
44000-byte size and BootServicesCode/Data metadata against the lease's actual
discovered identity. The complete object span must have actual EFI memory-map
coverage; image-owned sections permit BootServicesCode or BootServicesData,
while dynamic objects require BootServicesData. The EFI descriptor must expose
WB capability, no read protection/runtime attribute, and zero or identity
VirtualStart. Multiple cache capability bits are permitted; they do not prove
the current mapping mode. Malformed, overlapping, truncated, missing-WB or
missing descriptors fail. The fixed heap
windows are `[BD980000,D4E23000)` and `[D5100000,D8000000)`; their address alone
does not qualify an object. A fixed 64 KiB descriptor buffer avoids introducing
an allocation during read validation.

Test102 stopped at its first text address CFF0B000, before a Guard session or
target word read. Its old exact-WB EFI-map predicate did not log the descriptor,
so the captured log cannot establish that descriptor's complete Attribute.
The current correction follows actual Mu source rather than guessing those
missing bytes: `MdeModulePkg/Core/Dxe/Gcd/Gcd.c` 2548 supplies capabilities to
CoreAddMemoryDescriptor; `Mem/Page.c` 784/801 preserves Attribute during memory
type changes; CoreGetMemoryMap 1957 copies Entry->Attribute. Its 2295..2308
comment describes this capability-derived field, and the final loop clears
EFI_MEMORY_ACCESS_MASK, retaining the cache capabilities. The real DXE_Heap
resource value 703C07 converts to UC/WC/WT/WB capability bits together. Other
capabilities can remain in the raw Attribute as well. The reader therefore
requires the WB bit; it does not require the entire cache mask to equal WB.
Current cache admission still requires actual GCD exact WB and PAR FF in the
real guard. `ArmPkg/Drivers/CpuDxe/AArch64/Mmu.c` 321..353 describes the separate
page-table walk that synchronizes actual attributes into GCD.

The source evidence used by the host fixture is Page.c SHA256
`ee184898ae115d869d7ef59488087f53d467703ac3fa2ac4d1b6b640e392a7fc`,
Gcd.c SHA256
`3d82f7ff075dc491c8934979e8153dfb112fcab39148a4559a54c301c7dd034a`,
and AArch64 Mmu.c SHA256
`173073a5a9e827be71aa0222e7af26c6122d159eb314f79bfcc07c6c95d3b533`.
`tests/piano_mu_map_fixture.py` compiles the actual conversion table/function,
actual CoreGetMemoryMap descriptor-emission lines, type handling, and final
access-mask clearing. The storage/list boundary is a host fixture. This proves
mixed EFI capabilities plus current GCD WB/PAR FF are accepted, while current
GCD WC or PAR44 are rejected.

Each actual `PianoGuardedRead` Begin authorizes only the bounded object span,
with GCD SystemMemory/WB and actual read AT/PAR identity/attribute FF on every
covered page. Read uses the actual short LDR/fixup adapter. End runs before any
native Get/Enable/Disable/IsOn call. Identity and EFI coverage are checked again
after clean End. A one-byte Client flag read expands to its containing aligned
word inside the 32-byte object; it never grants adjacent heap access. Output
stays unchanged until the complete read, End and rechecks succeed.

The pin's actual object layout is:

| Role | Proven origin and size | Allowed dynamic request |
| --- | --- | --- |
| BSP Global | Static image data 283A0; image slot 3FE48 | Image-owned data |
| Modules | Static 28308, 9 pointers | Image-owned data |
| GCC Module | Static 28678, array 32418, count 149 | Image-owned data |
| GCC Node / Parent | Array +112*actual getter index must equal 33A68; name 14E02; parent 37528 | Image-owned data |
| INTERNAL Client | Two stable image slots 3F5F0 and 3FED8, plus DAL registry membership; 32 bytes | Only byte +19 |
| DAL registry entry | Image root slot 3FED0; native 32-byte entry, id0 and name 25533 | Internal verification only |
| Client reference | Node +58 head; native GetID allocates 24 bytes | +0 next pointer, +8 Client pointer, +10 four-byte ordinary/alternate counters, with counters limited to the validated Client |

The getter's actual typed ID supplies provider and index. The reader does not
return a hardcoded ID. The static BSP, array, GCC module/count and exact GCC
node must still match the pin. An alternate BSP/module/parent pointer is an
unknown object and fails; no unknown pointer opens the rest of a heap. Static
image data remains readable within the independently validated owned section.

Producer/lifetime evidence is from the complete pinned native PE, with all
addresses expressed as RVAs: 8A4C writes the incoming BSP into 3FE48. 8C60
passes RootDrvCtxt+98 (3FED8) as the output slot; 8C6C invokes attach 86F0 for
id0 and `INTERNAL` at 25533. Registry lookup 8730..874C follows 32-byte entry
next+0 and id+8; 8770 creates an entry, 8880 creates a 32-byte Client. 88C0
stores Client+10 backpointer, 891C..8924 links it into entry+18, and 8930
returns it through the output slot. Protocol entry 1DD0/1DF8 copies 3FED8 into
3F5F0. The adapter verifies both slots, registry id/name, Client membership and
backpointer. Its private registry and Client walks each reject a cycle or more
than 64 nodes. Those verification reads cannot be requested through the public
callback as a generic registry/Client reader.

GetID 5148/5190 allocates a 24-byte reference, 51D8 zeroes it, 51E4 writes the
Client pointer and 51F0 links it at the Node's ClientList head. Disable
5320..5554 reduces refs without unlink/free. The read therefore uses the actual
native producer's bounded object lifetime rather than inventing an allocator
registry requirement. Its Client reference walk separately rejects cycles and
more than 64 nodes. Every pointer/control graph is read twice coherently and
resolved again after the target read; graph or producer-anchor drift rejects
the output. This is narrow CPU read admission, never an allocation or DMA lease.

The existing Root CPU EBS fence must call `PianoDisplayClockReadFenceExit`,
including before Initialize if EBS arrives then. Guard sessions also own their
real short EBS event. Any lost services, active/owned/fatal/retained guard
state, nonexact End, or successful Begin with a null token retains the reader
and prevents another read/Close. No EFI call occurs after a fence. After an
early clean failure or successful lease Release, Root calls
`PianoDisplayClockReadClose` and requires exact success. A retained reader
must stay in driver-lifetime storage; it cannot be retried or unloaded.

`Report.EfiMap` preserves the last actual map decision: Status/GetMapStatus,
Reason, Cursor, descriptor index/type/base/pages/raw attributes/virtual address,
map bytes/descriptor bytes/version/key and overlap counterpart. Initial
Status/GetMapStatus are NOT_STARTED. Reason values are 0 not observed, 1 ready,
2 services lost, 3 GetMemoryMap result, 4 format, 5 invalid descriptor, 6 overlap,
7 no coverage, 8 wrong type, 9 missing WB capability, 10 read protected,
11 runtime and 12 nonidentity virtual address. Raw descriptor fields are
diagnostics; they never certify current cache or memory ownership.

Root may bind the actual `PianoDisplayClockReadFailureEvidence` callback to the
lease. A private reader sequence and last-call tuple must identify its very
first 256-byte text read at image+1000. That call must have returned actual
NOT_READY at the EFI map check, with zero sessions/guard reads/owned handlers,
no retained/lost/busy state, an authenticated still-owned FV copy and a live
CPU fence. The getter performs no EFI call and does not admit a memory read.
Wrong address/bytes/status, later calls, Close, EBS, an attempted guard or
unknown/partial state cannot produce this clean refusal evidence. It permits
the lease to free its own FV copy before GetID/Enable/event creation. Root then
closes the reader's separate FV copy and requires exact success. Free warnings
or EBS retain the state. This cleanup neither acquires a native reference nor
marks display hardware ready.

Run `python -m unittest discover -s tests -p test_display_clock_read.py -v`.
The host suite links the actual Reader and GuardedRead, uses the real pinned FV
bytes/relocations/static BSP table, and injects only the EFI/CPU/LDR boundaries.
It covers source/text drift, field bounds, stale identities, EFI/GCD/PAR drift,
unknown/overflow objects, two-root mismatch, typed module/Node drift, registry
and Client membership/cycles/64 limits, reference drift, partial protected
faults, EBS, foreign handlers and uncertain cleanup. Null-token and inconsistent
End results are fault injections around the real guard boundary. Separate
AArch64 checking copies the exact headers/source into a flat staging directory.
The Reader suite now has69 ASAN/UBSAN cases plus AArch64 flat staging. The
separate actual pipeline has20 UBSAN cases, compiling actual Lease/Reader/Guard
and verbatim Root GCC/startup helpers together. It verifies full pin/live text,
first native24-byte entry, refs+1/-1, enabledTRUE with legitimate HWCG onFALSE,
mixed capabilities, exact freeing of both FV copies with known-no-side-effects,
and retained free-warning/EBS/guard-uncertainty paths. Its actual private object
comparison is tested with poisoned padding: only Role/Base/Bytes/Anchor are
compared; genuine field drift still fails. Pipeline uses UBSAN because ASAN's
x64 shadow gap occupies the real low-heap fixture VA needed by both modules.
The independently instrumented modules retain their ASAN coverage. These are
host fixtures; successful physical acceptance of the corrected image remains
Root's next uniquely built-device check.
