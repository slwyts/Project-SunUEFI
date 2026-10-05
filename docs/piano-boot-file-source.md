# Piano readonly boot-file source

`bootprofiles/os-boot/PianoBootFileSource.c/h` is a real UEFI Simple File System
reader and CPU staging owner for the autonomous OS loader. It is independent of
the current product build inputs. It does not start an image, change the EFI
memory map, request ExitBootServices, or write storage.

The caller selects an existing `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` handle and a
configured absolute path. `PianoBootFileLoad` calls that handle's real
`HandleProtocol`, `OpenVolume`, and `Open(EFI_FILE_MODE_READ, attributes=0)`.
Paths start with a single backslash and reject traversal, empty segments,
wildcards and alternate-path syntax; no directory enumeration or search occurs.
Kernel, DTB and initrd can use three independent states with
`PianoBootFileLoadBundle`. This does not discover a partition or decide that a
particular volume is an approved OS source; those are the supervisor's decisions.

## Read and validation contract

The source requires caller-owned zeroed producer-lifetime state, APP TPL, real
Boot Services and a CPU-only caller lifetime fence. It registers its own
ExitBootServices fence before opening the volume. A query for `EFI_FILE_INFO`
must return exact `EFI_BUFFER_TOO_SMALL`; one bounded record of at most 4 KiB
must then return exact success. The record must describe a nonempty regular
file, contain a bounded terminated filename matching the configured leaf, and
have consistent record and file sizes.

One `EfiLoaderData` allocation holds the whole file. Reads use explicit offsets
and at most 64 KiB per chunk, with `SetPosition`, checked `GetPosition`, exact
`Read`, and checked final position. Short reads, warning statuses, changed
provider identity or method pointers, bad positions and malformed metadata are
rejected. The final metadata snapshot must agree with the initial one; access
time alone is ignored because readonly providers may update it during a read.
An extra EOF read must return success with zero bytes. Incremental SHA-256
covers all staged bytes; an optional caller-provided trusted 32-byte digest must
match. Without that pin, the computed digest identifies the staged snapshot and
does not authenticate its origin or establish that the file was unchanged before
the read began.

The file and root handle must both close with exact success before the source
becomes ready. There is no filesystem access through a ready source's exported
read or view methods. The caller's `BootServicesAlive` callback must remain
CPU-only after the UFS/device owners are retired.

## Shared source and loan lifecycle

`PianoBootFileExport` returns the existing `PIANO_BOOT_SOURCE` reader and
`PIANO_LAUNCH_BLOB` interface. `Take` produces an opaque unique ownership token;
`BorrowView` loans a stable range in the staging allocation without copying the
whole image. Tokens share a monotonic sequence across instances and cannot be
reused after restore. A current loan blocks `Restore`, `ZeroRelease` and
`PianoBootFileDispose`. The consumer must unborrow after all image, LoadFile2 and
FDT consumers are finished, then zero-release, or restore unchanged ownership
to the caller. An untaken snapshot can be disposed directly.

Output ranges for reads, export, owner tokens and view/loan tokens must not
overlap the producer state or staged data. Export outputs and view/loan outputs
also must not overlap each other. Address wrap is rejected before writing any
output. This preserves the snapshot against accidental destination aliasing;
the protocol still assumes cooperative EFI clients honor borrowed `CONST` views.

Release closes the local lifetime event, overwrites every staged byte through a
volatile store, and calls the real `FreePool`. Unknown ownership, provider
mutation, an error/warning during close/free, or a lost Boot Services fence
retains the state and refuses a retry. The state must not be overwritten or freed
while retained. A bundle failure disposes earlier completed snapshots when that
cleanup is safe and zeroes the bundle's exported interfaces; per-state retained
status remains available to the supervisor.

Current limits are at most three files and **64 MiB total configured budgets**.
The sum of `MaxBytes`, rather than the eventual file sizes, must fit the supplied
budget before any file opens. This conservative bound is compatible with the
current low-memory product. No 1 GiB allocation or high-DDR ownership is claimed.

## Integration and verification

The autonomous supervisor still needs to bind the approved SFS handle, fixed
paths/digests, lifetime context and budget, and pass the three exported blob
interfaces to the Linux EFI session. It must finish loading before retiring UFS,
then retain the CPU snapshots until the session releases its loans. This reader
has not been connected to the product Core or exercised on the tablet in this
change. Presence of the source is not evidence of successful autonomous boot.

Run:

```sh
python3 -m unittest discover -s tests -p test_boot_file_source.py -v
```

The test compiles the actual C reader and shared interfaces with AddressSanitizer
and UndefinedBehaviorSanitizer, uses real incremental SHA-256, and runs 29
isolated C cases. It covers exact offsets and closed handles before CPU reads,
metadata/size/name/digest failures, EOF growth, warning/error close/free, provider
change, EBS during a read, bundle budget and rollback, active/stale loans, zero
before free, and all state/data/output aliases. The SFS/Boot Services boundary is
a host fixture; these are not physical UFS, hardware DMA or OS boot tests. The
same actual source also passes strict AArch64 freestanding compilation.
