# Bounded NPA/VCS object observations

`uefi/components/display-rail/PianoDisplayRailObserve.c/h` implements Init, Observe,
CPU-only Reemit, Close and retained/report getters. The environment receives the
actual private Clock reader plus explicit NPA/VCS handles from NativeProbe's
successful GUID→StartImage registry. There is no loaded-image size scan, generic
address reader, native NPA/voltage request, clock call or register access.

Init owns two FV PE copies, checks their complete SHA256 pins and verifies the
corresponding explicit LoadedImage base, size and BootServicesCode/Data types.
It normalizes only known DIR64 literals in the owned copies and compares live
code with protected 32-bit reads. NPA's installed protocol must be exactly its
image+110B8, revision10003, with +1B0/+1B8 functions1AE0/1AF8. VCS code and its
typed backend table have independent image identity. Every CPU read requires a
bounded known-low-heap span, non-overlapping valid EFI map coverage with WB
capability and appropriate BS code/data type, then actual Guard SystemMemory/WB,
identity/PAR FF qualification. Every successful Guard Begin attempts exact End.
No MMIO or general low-heap permission is carried by this API.

The exact native pin identities are:

| Image | Complete PE SHA256 | Bytes | Protected code interval |
| --- | --- | --- | --- |
| NpaDxe | `19a7df5cdf25ae247ca5628bd67496729f17f30d74e564a7d7f999ac2d5934b9` |13000 hex |1000..DDAC exclusive |
| VcsDxe | `d335dbcfd4175e6d9f189062bc777961f497e968d66acc7dacb4aae8b3055e4f` |11000 hex |1000..7F4C exclusive |

Observe invokes the actual fixed non-GDSC selector API before and after two
complete object graphs. It starts from the selector's real MM/MX clients; it
does not accept caller booleans or a caller-selected CPU address. MM must agree
across the two graphs. MX is optional diagnostics and supports the actual UEFI
DT's primary `/vcs/vdd_mxa` with alias `/vcs/vdd_mx`. It is not an additional
mandatory power vote for the eight DISPCC control reads, whose minimum source
dependency is MMCX plus interface AHB. DPU/DSI operation has a separate contract.

Client addresses can be four-byte aligned. Test104 observed MM D00F16C4 and MX
D00F1594. The collector loads only aligned32-bit words and reconstructs fields
with CopyMem, so it never executes an unaligned64-bit native load or rejects
these genuine allocator outputs. No eight-byte pointer-alignment assumption is
introduced. Objects and names are derived through typed links; a malformed,
unmapped, changed or foreign image/table pointer stays a real failure.

Test105 established live NPA/VCS FV/code identities, but stopped during the first
MM graph with NPA request/applied48, pending56 and Clock cached0. VCS applied was
not read; its zero-filled report field was not a voltage result. The old logs did
not identify which subsequent field/map admission failed. Each graph now saves
its last field/address/length/result, actual EFI descriptor type/base/pages/
attributes and rejection reason, and the last Guard mapping/end evidence. Saved
Definition/Node/Plugin pointers and an independently named DRV config/id/handle
line identify the typed producer chain. Logs remain bounded complete short lines
and reemit stored CPU data. Unattempted fields use NotStarted rather than zero
status/SUCCESS, including a missing optional MX client. Diagnostics are excluded
from the semantic two-graph comparison: descriptor index/key drift cannot make
unchanged object data appear inconsistent.
Name comparisons retain only the matched string through its NUL; the unused
tail of the16-byte physical read is zeroed before saving the semantic graph.
Changing adjacent bytes after the NUL therefore cannot create graph drift.

One precisely proved static producer is allowed to have BSCode or BSData ledger
classification. This is not a rule for every image or every low-heap address.
VCS60F0/60F4 sets the private context atFA30 toA468;6104/610C sets the rail-array
pointer atA478 toA488. The current instantiated count is atA480. After guarded
reads prove these exact anchors and a count1..20, a typed field must lie entirely
inside a currently instantiated0x120-byte rail slot. Native6158/6168 selects
those slots.6734..6994 places the Node atslot+38 and Resource Definition atslot+78.
The20 possible slots end atBB08. Matching initialization at6314/631C/6320 places
each rail's backend context atBB08+i×328 and its DRV config atcontext+290. The
collector allows only context+10 pointer and context+290..2A0 data fields, then
checks the rail/context/config ordinal relationship. It does not expose the
whole backend context. The actual handle isconfig+8, which iscontext+298;
context+10 is the config/DRV-ID pointer, not the handle. Unknown image data,
uninstantiated rows, straddling fields, foreign anchors and dynamic low-heap
BSCode remain refused. GCD WB/PAR FF, code pins, fresh loaded identities and
exact Guard End requirements are unchanged. Next live capture must determine
whether105's failed field actually belongs to this producer; host cases prove
the intended boundary, not the tablet's resulting power state.

## Actual binary field evidence

These offsets are from the pinned binaries, not a layout invented by fixtures.
The independent disassemblies are `build/NpaDxe.disasm` and
`build/VcsDxe.disasm`; each collector fixture uses the same complete PE bytes.

| Field/link | Offset | Actual instruction evidence |
| --- | --- | --- |
| NPA client→resource |20 |NPA3DC8 and3ECC use the pointer to access the resource/plugin |
| Client type |30 |NPA3DD0/3DDC checks40 for required; other branches handle800/8000 |
| Active request index |68 |NPA3EE0 and3F14..3FA8 toggle/compare the two request slots |
| Active/pending request |38+24×index |NPA3EEC/3F70 place requested or mapped scalar state in the other slot |
| Client request callback |88 |NPA4474/4480 dispatches it during single-client batch commit |
| Resource→definition/node |0/10 |VCS6AAC/6AB4 and6AC0 access definition/node data |
| Node driver/data |8/18 |VCS node callback6A8C;6AB4 loads node data and6ABC verifies rail+B8 backlink |
| Resource plugin |28 |NPA3EC8 loads plugin before its optional request mapper+30 |
| NPA aggregated state |30 |NPA994C stores update return; not a voltage acknowledgment |
| Required/suppressible aggregation |40/44 |NPA98AC/98C0 reads these values, and VCS6B70 reads the same pair |
| Resource definition→rail |28 |VCS6B08 obtains the rail from the definition data |
| VCS rail→resource |B8 |VCS6ABC/6AC4 checks it matches the caller resource |
| Rail diagnostic name |78 |VCS6B74/6CD8 use it for actual resource/rail logging |
| Rail backend table |20 |VCS6C34/6C3C dispatches backend set-corner+8 |
| VCS applied corner |30 |VCS6BA8/6BD0 reads it; RPMh set-corner7218 stores new state at7478 |
| Backend context/config/handle |rail28→context10→config8 |VCS711C/7134..7170 resolves/creates the RPMh handle through this chain |

The live plugin must be VCS+98D8, update VCS+6D68, supported mask844 and no scalar
request mapper at+30. Those exact fields are read, not inferred from a successful
callback. Consequently the observed parent corner and NPA client scalar values
use the same unmapped raw unit for this instance; comparisons still describe
software state. The backend must be VCS+A2C0 with init711C/set-corner7218. Its
current context/config/DRV-id/handle are recorded, while RPMh completion remains
unknown. `PowerReady`, `MemoryOwnershipGranted` and `RpmhCompletionObserved`
always remain false. Clock cached/config corner, client request, NPA aggregation
and VCS applied corner are separate fields, even when their numbers agree.

## Lifetime and integration

State is resident for the driver's lifetime and cannot be restarted in place.
Init, Observe, Reemit and Close acquire Busy before any EFI or Alive callback.
Reentrant operations return AlreadyStarted without allocating, freeing or
changing the active operation. Its own ExitBootServices event sets a CPU fence;
every BS/DS boundary checks the fence before and after access. A retained source
Reader propagates into this observer. Guard cleanup uncertainty, partial FV
outputs, warning frees/event closure and EBS preserve the resident state and
prohibit retry or cleanup. Root must halt on retained/lost state.

Clean Init refusals can be followed by Close to free both successfully owned FV
copies and close the exact event. Close failure is observable and must prevent
parent clock release. Root's planned order is child clock retirement → RailClose
→ GCC release → ClockReaderClose. Replay uses saved CPU snapshots only. There
are16 phase slots, sufficient for acquire-before/after and release-before/after;
an exhausted report does not begin another source or Guard operation.

Run `python3 -m unittest discover -s tests/unit -p test_display_rail_observe.py -v`.
The actual collector and actual Guard, complete native NPA/VCS PE pins and real
BasePrintLib pass48 fork cases plus strict AARCH64 compilation. The tests cover
four-byte-aligned links, code/hash/handle/protocol identity, true graph drift,
bad callbacks/backlinks/plugin, optional MX/alias, EFI/GCD refusals, read abort,
uncertain guard cleanup, EBS, failure cleanup and Init/Alive/selector/Close
callback reentry. Real AsciiVSPrint256 checks complete short lines. Cases also
exercise the authentic static rail/context layout when the complete VCS
allocation has BSCode type, bad producer/count/ordinal/row boundaries, unchanged
dynamic-heap Code rejection, real EFI/Guard refusal diagnostics and diagnostic
drift/name-tail changes excluded from semantic equality. The fixture
substitutes the selector and machine/EFI service boundaries; native NPA/VCS code
is never executed and no fixture authorizes a power lifetime. Low physical
addresses are used unchanged, so UBSAN is used here because x64 ASAN reserves
that address-space gap; the separate actual Guard/Reader suites retain ASAN.
No tablet, firmware build or native rail mutation is performed by these tests.
