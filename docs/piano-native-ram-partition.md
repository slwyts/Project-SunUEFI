# Native Piano RAM-partition provider and product inventory

The real provider is **EnvDxeEnhanced**, already loaded in product/probe APRIORI
and FVMAIN. It is not a missing member of the 13 manually started foundation
drivers. Original UEFI FFS GUID `90a49afd-422f-08ae-9611-e788d3804845`; original
PE bytes 77824, image size `0x13000`, SHA256
`593d9e766c0070e01d4c6684b7bb8050f32f77ea3ca300c6a110ad3903de17e3`.
`upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.efi`
is byte-identical to the captured original.

Original UEFI GUID-byte scan found this RAM GUID only in EnvDxeEnhanced and
QcomBds. Original ABL LinuxLoader also contains it as a consumer. Env's actual
RVA `0x2520` init, called by main init at `0x289c`, invokes Boot Services offset
`0x148` (InstallMultipleProtocolInterfaces) with RAM protocol GUID at
`0x10178`, interface at `0x102e0` and NULL terminator. A failed RAM-list setup
can bypass installation; binary presence is not runtime presence proof.

The GUID is `5172ffb5-4253-7d51-c641-a701f973103c`. Both native and the older
QcomPkg header claim revision `0x10002`, but **the header is not the native ABI**:

| interface offset | native function RVA | audited meaning |
|---|---|---|
| 00 | constant 10002 | revision |
| 08 | 2268 | GetVersion; writes UINT32 major=2/minor=0 |
| 10 | 228c | GetHighestBankBit; UINT32 output |
| 18 | 22f4 | GetRamPartitions(This, entries, UINT32 count*) |
| 20 | 22a8 | MinPasr method; not invoked by new inventory |
| 28 | 23d4 | total-size-like UINT64 output method; not invoked |
| 30 | 2420 | GetPreloaded(This, entries, UINT64 count*) |

Native bank output is 16 bytes `{UINT64 Base, AvailableLength}`. Native
preloaded output is **24 bytes** `{UINT64 Base, Size; UINT32 RawType, Padding}`;
padding is not written by native code, so caller initializes it. There is no
name string in the native public output. QcomBds at `58f8` and `5930` independently
calls interface `+0x30`, allocates count×24 bytes, advances 24 bytes and tests
the raw type at +16. The old header's preloaded method at `+0x28`, count32 and
48-byte named entry must not be called on this device.

GetRamPartitions at `22f4..23d0` queries the cached native bank list via `8778`,
checks capacity, allocates a low BootServicesData temporary 72-byte-per-entry
array with BS AllocatePool, and returns only base/available length. GetPreloaded
at `2420..251c` uses the other cached list via `8878`, counts in 64 bits, checks
capacity, then copies base/size/raw type. Both helpers use cached Env arrays
(32×72 bytes), not a target DDR/MMIO probe. They do not map RAM or initialize
another peripheral. Native error paths can leave scratch allocation ownership
unclear, so the caller does not retry a failed native operation.

Important provenance limit: Env setup `8a74` reads real RAM partition data from
SMEM item402 via `8638` (checks `9da5e0a8/af9ec4e2` signatures). If unavailable
with NOT_READY, `8b00..8b48` creates a **synthetic fallback** single available
bank `[80000000,e0000000)` of 1.5GiB. GetVersion always writes 2/0 rather than
proving an authentic SMEM source. Successful getter/revision/version therefore
cannot grant DDR ownership. Inventory reports `PotentialFallback` for that
exact range and keeps `OwnershipVerified=FALSE` for every result.

## Product integration

The canonical implementation is `bootprofiles/uefi-app/PianoRamPartition.c/.h`.
The single ProductCore calls the identity-only inventory after the native
foundation is ready. Only exact successful image/ABI verification permits one
explicit bounded native fetch. Retained resources or a services-loss result
fail-stop; ordinary missing-provider/ABI/data errors are logged as unavailable
and do not pretend that high RAM is ready. The actual product source includes
this module; it is not an alternative test image.

`PianoRamPartitionInventory(FALSE, &Report)` performs only presence and ABI
identity checks. It does not call any native Get function. The binding requires:

1. The exact FV Env PE full SHA and size.
2. A live installed LoadedImage with the actual Env FV-file GUID, aligned base,
   exact image size and interface address `ImageBase+102e0`.
3. Actual loaded `.text` matching the pinned source, normalizing its audited
   DIR64 pointer literals. This PE does have DIR64 literals inside `.text`;
   comparing raw unrelocated bytes would reject the real loaded image.
4. Exact revision and every one of the six native method RVAs. Revision alone
   never permits a call.

Only explicit TRUE fetches native banks and preloaded inventory. It uses one
query and one bounded read (at most 32 records) for each getter; no resize loop.
Fresh protocol/image/vtable identity is rechecked before each full read and at
the end. Warnings, count changes, overflow, duplicate/overlapping bank ranges,
zero-sized entries or identity replacement are rejected. Raw preloaded type is
retained as an uninterpreted native value. No actual target RAM allocation,
memory mapping, DDR pattern or MMIO write exists in this module.

Temporary FV source and handle arrays are released with exact-success BS
FreePool checks. Unknown release/EBS retains their tokens and blocks retries.
Root must retain a failed report and follow its owner/lifetime policy; it must
not retry release of uncertain tokens or treat inventory success as memory
allocation permission.

The product INF already binds MdePkg/CryptoPkg, UefiBootServicesTableLib,
DxeServicesLib, BaseMemoryLib, BaseCryptLib and LoadedImage protocol. This source
does not use the old EFIRamPartition.GetPreLoadedImageTable field. Calls are at
APP TPL after Env dispatch, before high-memory mapping. Runtime results use
`PIANO_PRODUCT_RAM_ABI`, `PIANO_PRODUCT_RAM_INVENTORY`, `RAM_BANK` and
`RAM_PRELOADED` log records. Every record keeps ownership unproved; current
download capacity remains 64 MiB.

## Host verification scope

Pinned PE section layout has RawOffset==RVA for .text (1000), .data (10000) and
.reloc (12000), SizeOfImage==file bytes==13000. Both native NULL/count0 queries
return BUFFER_TOO_SMALL when their cached list is nonempty, or exact SUCCESS
with count0 when empty; they return before scratch allocation or output record
copy. Full fetch uses distinct UINT32 bank and UINT64 preloaded counts. A final
fresh-identity error after successful reads keeps raw observations but sets
DataValid FALSE; OwnershipVerified remains FALSE in every report.

`tests/PianoRamPartitionTest.c` executes the actual C inventory against the real
captured Env PE. Actual source SHA, relocation normalization, loaded-image
identity and vtable guards run unchanged. Only the **audited ARM method call
boundary** is replaced, because the host is x86. This is not provider/hardware
validation. ASan/UBSan cover 27 fork cases: default-zero Get calls, valid bounded
inventory, missing/changed PE, wrong image/path/text/revision/method, oversized
counts, warning/error, stale identity, changed counts, overflowing or overlapping
banks, synthetic fallback, zero preloaded size, EBS and uncertain release.

`python3 -m unittest discover -s tests -p test_ram_partition.py -v` executes the
27 cases and strictly compiles the actual ARM64 ABI. The ARM call boundary is
substituted only in the host test. Native invocation, real collected bank data,
high-memory ownership, MMU/GCD integration and 1 GiB allocation still require
product device acceptance. No provider is synthesized from DT or registered by
this module, and no high-memory mapping/allocation or target MMIO write occurs.
