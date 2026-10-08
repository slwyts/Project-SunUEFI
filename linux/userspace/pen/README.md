# Piano pen algorithm worker

`prepare_alg_gnu.py` creates a linkage copy of the user's exact factory ALG
(SHA `26f86f…f1c9`). It leaves the original and executable instructions intact,
changes the three GNU library names, removes Bionic `LIBC` version requirements,
and binds seven imports to the worker's real mutex/stdio/checked-string adapters.
No proprietary library or ini is included in this directory.

`piano-pen-worker.c --config ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin` reads the
external ini and the actual hardware config prefix, then calls the verified ALG
normal/stylus configuration readers, context construction and stylus init/exit.
It does not load HAL startup, open a device/FIFO, solve a frame or generate
uinput events. The actual ARM64 GNU configuration/context/stylus init and exit
completed on2026-10-08 in a no-network, read-only Bubblewrap/QEMU run with an
empty `/dev`, using the captured factory ini and hardware header. It returned
resolution100,213600×320000,columns60/rows40 and stylus enabled1. No pen frames
or uinput events have been tested.

Two real failures were corrected: retaining `DT_VERSYM` after removing
`DT_VERNEED` made GNU ld.so dereference an absent version table; verbosity0
still called an ALG level0 logger through NULL. The copy now removes both
version tags, and the worker uses the library's signed negative verbosity to
disable logging. No dummy callback or suppressed error result was added.

`piano-pen-frame.c` now reconstructs the proven type29 pen prefix in an owned
1297-byte internal object. It reads geometry from the external ini and the
actual9-byte hardware header, checks both original additive checksums and
complements, copies four signed16 arrays and the20-byte trailer, and retains
the real four-part hand accumulator. A sum of10 alone cannot publish a hand
matrix unless all four quarters were actually filled. Unaligned pointers refer
to buffers owned by this object; they never point into a transient SPI packet.

`--decode29 ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin RAW29.bin [...]` decodes
captured original SPI packets in order. `--prepare29` additionally requires a
real1032-byte common7/id0x440 record before the raw packets. It delivers that
unchanged record to the actual pressure-ring callback, then calls ALG slot8
to sign-extend the real four matrices into `stylus_total_data`. It reports
preparation only: no solved coordinates, physical events or pressure accuracy.
No real29/common record has been captured or processed yet. The updated worker
was compiled for ARM64 and its original isolated configuration/init completed
again; no fabricated frame was used to test the new path.

Do not pass this partial object to the complete ALG core or its
`parse_data_package`: the latter also copies main-touch/SC data through internal
pointers0x7c/0x84/0x8c and updates frame/noise state. This adapter provides the
main-touch quarter accumulator but has no actual SC history. Those inputs and
the active pen profile must be bound from the existing single stream owner
before enabling full slot7 processing. Slot7 does call its report constructor;
its absent callback is guarded, but that does not replace missing input state.
Type17 is preserved by the factory v2 decoder and issues an external ALG/HAL
command; it is never changed into29. Any frequency-hopping request from type29
is returned as the actual metadata byte, without hardware command or fake ACK.

Next collect one short synchronized sequence of actual29 packets, the genuine
common pressure/profile records and factory final events; no second FIFO
reader is needed. Bind the verified worker output into the owner's standard
Linux pen uinput/Wayland reporting after comparing this sequence.

Configuration/current ABI evidence and build hashes:
[pen protocol](../../../docs/devel/piano-pen-protocol.md).
