# Early SMEM RAM402 reader contract

`bootprofiles/early-memory/PianoSmemRam.c/.h` is an actual read-only source
collector and payload parser. The product now calls it through the guarded DXE
reader after Foundation and before UFS/USB startup. This runtime binding has
not been exercised on the tablet. It does not publish memory HOBs/GCD/MMU
changes or authorize DDR use, and is not a first-SEC-memory provider.
`EFI_SUCCESS` means that the supported data was parsed after two matching
observations. It does not mean full DDR, reserved-region ownership, secure access,
or bootstrap readiness has been proved.

## Calls and storage

`PianoSmemRamParse(Payload, Bytes, Report)` parses existing CPU bytes only.
`PianoSmemRamCollect(Reader, Work, Report)` accesses physical addresses solely
through the caller's `TryRead`. The callback must provide a bounded complete
copy, recover synchronous read exceptions, and return exact `EFI_SUCCESS` only
for a complete copy. Warnings are not accepted as successful reads. There is no
native SMEM initialization, BootServices allocation, remote lock, MMIO write,
physical-memory scan, fallback bank, or direct pointer dereference.

The caller must zero-initialize and retain `Work` in **producer-lifetime static
storage**. Its two 8 KiB raw buffers and 32 KiB metadata trace must not be placed
on the SEC stack. `Report`, `Work`, and `Reader` must be disjoint. Calls are
serialized; same-workspace reentry returns `EFI_ALREADY_STARTED` without changing
the current report. Invalid/aliased arguments likewise leave output untouched.
The callback/context must remain valid and must not mutate
these objects or reader limits during a call. There is no global state or hidden
allocator. On failure, `Parsed`, bank/preloaded counts and arrays are cleared;
observed version/address/CRC/read counters remain diagnostic, not usable results;
the parsed non-SDRAM category count is also cleared.

Caller limits are nonzero and capped at 2,048 callback attempts and 256 KiB total
bytes per call; each callback copies at most 256 bytes. The `$PRT` walk is also
capped at 512 item headers and the `$TOC` entry count must fit its final 4 KiB
page. `EFI_TIMEOUT` reports budget exhaustion. Individual callback execution
time is a platform obligation; this source cannot recover a callback that hangs.

## Fixed address and format limits

All SMEM reads must stay in `[0x81D00000, 0x81F00000)`, the 2 MiB region confirmed
by the product static map, board DT, and current Android DT sysfs. Only two other
addresses are accepted: 4-byte reads at `0x1FD4000` and `0x1FD4004`, the native
Env cookie low/high registers. The combined cookie is recorded twice when
readable, but **SIII is unresolved** and its value never authorizes a new range.
Cookie access failure/change is reported separately and does not convert a
valid fixed-window RAM402 snapshot into proof of a cookie-derived source.

The SMEM initialized field at `+0xC0` must be 1 and reserved `+0xCC` must be 0.
The SBL version is read at `+0x5C`; only major 11 and 12 are supported:

- Major 11 reads item402's 16-byte TOC entry at `0x81D019F0`. Allocated must be
  1; offset/size must be 8-byte aligned, after the legacy TOC, and contained in
  the validated heap free offset. `aux_base & 0xFFFFFFFC` must be zero or the
  fixed SMEM base; another region is explicitly unsupported.
- Major 12 validates `$TOC` at `0x81EFF000`, version 1, and a unique nonempty
  global entry with both hosts `0xFFFE`. Its `$PRT` header/hosts/size/free offsets
  must agree. The uncached list advances by `16 + padding_hdr + size`; the
  cached list descends by `ALIGN(16, cacheline) + size`, with data preceding its
  header. Size/padding/canary/partition boundaries are checked before accepting
  item402. Both lists are walked to their declared boundaries, and duplicate
  item402 entries are rejected. No other partition payload is read.

This layout follows the primary
[Linux qcom SMEM source](https://github.com/torvalds/linux/blob/v6.16/drivers/soc/qcom/smem.c),
also present in `kernels/linux-piano/drivers/soc/qcom/smem.c`. The collector
intentionally applies stricter size/alignment/budget and duplicate checks.
Unknown majors, partition versions, auxiliary regions, or shapes are rejected,
not treated as a usable fallback.

## RAM402 interpretation and evidence

Native Env disassembly identifies magic words `0x9DA5E0A8`, `0xAF9EC4E2`,
payload version at `+8`, count at `+0x10`, and entries at `+0x18`. Only actual
version 1 (64-byte stride) and version 2 (72-byte stride) are accepted, with
1..64 entries and at most 8 KiB of payload. Each SDRAM entry (category 14) has
Base at `+0x10`, RawSize at `+0x18`, RawType at `+0x2C`; version 2 also has
AvailableLength at `+0x40`. Version 1's reported AvailableLength is its raw size.
The native special subtraction of 2 MiB for a bank ending at 4 GiB is **not**
silently applied; the parser preserves raw claims for a later reservation-aware
consumer.

Type 1 is a bank; preloaded types are 5..8 for v1 and 5..9 for v2. Unsupported
SDRAM types reject the result rather than silently losing a possible reservation.
Non-SDRAM categories are counted and excluded. Nonzero sizes, 64-bit end-address
overflow, available length, count/stride and payload boundaries are checked.
Type1 raw bank extents must not duplicate or overlap one another. Preloaded
RawSize/RawType/SourceIndex are preserved; they are not interpreted as
free memory. Raw bank extents and preloaded ranges are not asserted to be
disjoint: establishing the real ownership/reservation relationship is a later
bootstrap requirement.

The collector performs two sequential full locator passes and two full payload
copies. The metadata trace records address, length and actual bytes, and is
compared **byte for byte**, as is the payload. `RepeatedMetadataEqual` and
`RepeatedPayloadEqual` expose these separate observations; `PayloadCrc32` is an
archive identifier, not the comparison predicate. Without the remote lock this
is not an atomic snapshot and cannot exclude change-and-revert between reads.
Unknown SIII/cookie provenance, live RAM402 contents, reserved owners, and the
early exception-recoverable physical reader remain platform integration work.

## Verification

`python -m unittest discover -s tests -p test_smem_ram.py -v` runs the actual C
source under AddressSanitizer/UndefinedBehaviorSanitizer and checks the same
source for AArch64 Windows/EDK2 syntax. The byte-addressed fixture covers 57 cases
including both RAM layouts, major11 and global12 cached/uncached paths, callback
failure/warnings, unsupported formats, overflow, duplicates, budget exhaustion,
exact metadata/payload changes, raw cookie failure/change, aliases and reentry.
These are source tests, not device or full-DDR validation.
