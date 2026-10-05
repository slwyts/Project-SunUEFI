# Product UFS dedicated-volume provider

This module is a long-lived dedicated-volume provider, not the test86 temporary
FAT format/restore routine. It contains no provisioning, GPT writer, formatter,
original-gap restore or caller `Authorized` flag. Current original GPT media
returns `EFI_NOT_FOUND` with zero WRITE and zero SYNCHRONIZE CACHE commands.
No writable protocol or NV interface is available in that state.

## Exact layout and real readiness evidence

The only container is LUN4, 4096-byte blocks, physical LBA375040 through378623
inclusive (3584 blocks, 14MiB). The immutable wire schema is documented in
`PianoUfsProductVolume.h`. Layout1 reserves:

| Container blocks | Purpose | Public access |
| --- | --- | --- |
| 0 and1 | Byte-identical volume identity headers | No write API |
| 2 through2047 | FAT child, 2046 blocks (8MiB minus8KiB) | Dedicated writable BlockIO |
| 2048 through2815 | NV journal slotA, 768 blocks (3MiB) | Private slot-relative callbacks |
| 2816 through3583 | NV journal slotB, 768 blocks (3MiB) | Private slot-relative callbacks |

The original three PC GPT blobs must match the independently pinned SHA256
values. Both live GPT copies must have valid CRCs and identical entry arrays.
All original entries and header bytes remain identical except the two CRC
fields caused by adding the single previously empty slot95. Its type GUID is
`3e4ea305-5b3d-49a4-b3b4-5844ef513648`, name `PianoUEFI Storage`, attributes2
(the standard no-BlockIO attribute), and exact full-container bounds. It must
have a nonzero unique partition UUID and overlap no original partition.

Both volume headers must match byte for byte, have valid whole-block CRCs,
zero reserved bytes and exact fixed layout/geometry. UUID matches the new
GPT entry, disk GUID matches the pinned original GPT, and type/layout match.
Fresh capacity, FUA support, mode/unit/permanent/power-on write protection and
actual owned UFS DMA/SMMU state are checked before access and mutation.
Neither a caller bool nor an old in-memory success can substitute for these
media reads. A UUID change during a live volume fences it.

## Actual shared transport and writes

`PIANO_UFS_PRODUCT_STORAGE=1` adds the private adapter inside the existing
`PianoUfsReadOnlyDma.c`. It reuses its three already allocated/mapped UCD,
UTRL and4KiB data buffers, existing owner domain and exact test86 wire builders.
It creates no parallel DMA allocator/domain. Data bounce becomes bidirectional
for this product gate; the six original LUN BlockIO media remain readonly.

Each request excludes the shared transport. HCI list bases, queue readback,
SMMU owner identity/faults, complete routing and peers, exact reserved WB buffer
shapes and software PA/IOVA translations are checked. The snapshot is frozen
only within that current lease; it does not assume the later USB lifetime is
identical to UFS's initial open snapshot.

Each4KiB write is marked pending before callback entry, uses WRITE(10) FUA,
requires exact GOOD/OCS/tag/LUN/count, performs full-container SYNCHRONIZE CACHE,
then issues a fresh READ and compares an independent RX buffer. RX is first
filled with the bitwise inverse of TX, so a callback returning stale untouched
bytes cannot pass even for an all0xCC payload. A final media gate confirms the
identity remains valid. Verified writes are persistent, not restored to zero.
Flush performs an actual exact sync and media/quiet checks.

Any transport warning, short/failed/unknown write, changed guard, failed readback
or uncertain release retains the owner and first failure, makes public media
readonly, revokes NV readiness and prevents reset/OS retirement. ResetBlock
cannot clear the fence. Bounds/TPL admission failures do not submit hardware.

## TPL and lifecycle

The standard Mu FAT filesystem holds FatFsLock at `TPL_CALLBACK`, and DiskIo
performs synchronous subtasks there. Public FAT I/O therefore supports APP
through CALLBACK. Standard VariableRuntimeDxe holds its variable lock at
`TPL_NOTIFY`; private NV I/O supports APP through NOTIFY. The lease never lowers
the caller's TPL: it raises low callers to CALLBACK, keeps NOTIFY as-is, and
restores only its own raised lease. It does not invoke WaitForEvent, the APP USB
worker or an allocator in these transport operations. Binding/publication occur
at actual APP. This is synchronous BlockIO, not an asynchronous BlockIO2 backend.

Only verified media can publish the distinct FAT child at handle slot7. The
original RO providers are retained. Typed UFS retirement disconnects that child
while live, permits its CALLBACK flush, syncs/closes the dedicated provider,
then performs existing DMA/domain/clock teardown. The exact child BlockIO and
DevicePath are uninstalled rather than using the original-LUN interface at7.
Seven parent disconnects and eight protocol removals are validated by the
integrated host case. The legacy Stop path refuses a published product child.

After Close/EBS, a stale NV token cannot access storage. This Boot Services
transport is not an OS-runtime UFS backend. A standard persistent FVB/variable
integration also needs correct DXE initialization order and its own EBS/runtime
fence before using these callbacks. Connecting a late callback to an already
initialized RAM variable driver does not prove journal restoration.

## Integration and validation status

The product core must call `PianoUfsProductTransportIo` after real RO UFS startup,
then `PianoUfsProductVolumeOpen` with pinned original GPT blobs. NOT_FOUND keeps
honest unprovisioned state. Only exact success permits
`PianoUfsProductTransportPublish` and `PianoUfsProductVolumeNvIo`.
The latter supplies the shared volume UUID/layout and fixed slot-relative IO
for the standard NV+FTW snapshot journal. Add ProductVolume and BoundedLayout
sources and the shared inline WriteGuard header when enabling the transport.
The product does not link the diagnostic restore-test transaction entrypoint.

`python3 -m unittest discover -s tests -p test_ufs_product_volume.py -v` passes
20 memory-backed provider cases plus an actual shared Submit/adapter suite:
original GPT refusal, reservation/UUID/CRC/pin mutation, span limits, FUA/sync/RX,
APP/CALLBACK/NOTIFY leases, readonly originals, strict peer/translation failures,
partial writes, guarded publication and exact typed child teardown. ASan/UBSan
and gate0/gate1 strict AArch64 checks pass. Existing bounded-volume and typed
reset/probe suites also pass.

This is offline implementation. The present product candidate has not bound or
physically verified this provider. No GPT/container provisioning or device write
was performed. A PC provisioning proposal and explicit permanent-reservation
approval are separate from this code; physical persistence, Shell file writes,
FVB/FTW integration and recovery remain acceptance work.
