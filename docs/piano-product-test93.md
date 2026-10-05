# Product Test93: white screen with live firmware/UI events

The unique product build `8f6347d8-1e76-485c-a6bf-4f3fbc20912f`, image SHA256
`990ac33e33581b4af735d1b05c674638fc4139a288c3a3b93395b73c98f4033a`, was sent
with one successful `fastboot boot`. No persistent-write commands were issued.
The operator reported a white screen without visible startup rows, then forced
a power-key restart. Android returned with boot-complete1. The final routine
26-partition hash check passed; subsequent RAM-only tests omit routine hash
checks at the user's request.

Host sysfs exposed USB VID/PID1209:8750. The host kernel journal places this
connection at03:21:18..03:22:16, about58seconds, and records product
`piano fastboot debug` and serial `SunUEFI-piano`. A standard fastboot version
query did not complete before recovery. Configuration/interface accessibility
was not captured, so full enumeration and command operation remain unverified.

The recovered `private/analysis/ramlog-test-93/console.txt` contains2097140bytes.
Its startup prefix has been overwritten by many UFS/DMA read records. The
collector formerly returned an empty `uefi.txt` when no early/session marker
survived. It now preserves the observed wrapped firmware/UI tail, explicitly
without inventing a session marker, CORE_READY line or cold-memory proof.

Observed records include successful BLOCKIO_READ/SCSI completions on LUN4,
SimpleInit's UEFI GOP resolution3200x2136 and completed driver initialization,
selected font sizes72/64, filesystem prober0items, product Setup/Shell icon
lookups, and real physical/menu key events. These are evidence that firmware
execution reached the UI path; white pixels alone do not prove a CPU halt.
Some primary log lines are damaged while paired DMA_COPY records remain
readable. The original binary is preserved unchanged.

The RAMv3 bank/preloaded/SIII startup records are unavailable in this tail.
No high-DDR ownership or large-buffer admission follows from this test. The
display publication path and long synchronous filesystem-probe/USB service
interaction are the next concrete investigations. Display_Reserved remains
the existingFC800000/2B00000 write-through-XN region; a missing explicit cache
publication operation is not itself proof of a live write-back mapping.
