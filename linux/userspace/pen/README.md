# Piano pen algorithm worker

`prepare_alg_gnu.py` creates a linkage copy of the user's exact factory ALG
(SHA `26f86f…f1c9`). It leaves the original and executable instructions intact,
changes the three GNU library names, removes Bionic `LIBC` version requirements,
and binds seven imports to the worker's real mutex/stdio/checked-string adapters.
No proprietary library or ini is included in this directory.

`piano-pen-worker.c --config ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin` reads the
external ini and the actual hardware config prefix, then calls the verified ALG
normal/stylus configuration readers, context construction and stylus init/exit.
It does not load HAL startup, open a device/FIFO, process a frame or generate
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

The per-frame path cannot take raw SPI bytes directly. The actual stylus
interface uses a HAL internal frame with embedded unaligned pointers; raw17's
path also issues an external command. Prove that adapter before using pure
interface slots7/8, then forward packets from the existing FIFO owner to one
worker. The owner keeps standard Linux pen uinput/Wayland reporting.

Configuration/current ABI evidence and build hashes:
[pen protocol](../../../docs/devel/piano-pen-protocol.md).
