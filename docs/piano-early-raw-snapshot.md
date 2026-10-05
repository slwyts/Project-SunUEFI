# Revision 2 cold diagnostic snapshot

The existing cold observer GUID HOB is now revision 2. It retains the exact RAM402
payload, bounded to 8192 bytes, and the exact SIII descriptor, bounded to 2048
bytes, plus their lengths, CRC32 values and diagnostic metadata. The public
observer, publisher and product replay APIs and GUID are unchanged. Both producer
and consumer use the same exact revision 2 size; revision 1, truncated, oversized
and duplicate HOBs are rejected rather than read as a compatible prefix.

SEC copies RAM bytes only after two complete matching payload reads and matching
locator metadata. SIII bytes require two complete matching descriptor snapshots
and an already stable cookie selecting an aligned address inside the fixed
`[0x81D00000, 0x81F00000)` window. Both copies are compared again before freezing,
and their CRC must agree with the collector metadata. Partial, failed or unstable
reads leave zero raw length, CRC and coherent flag; unused array bytes stay zero.
The existing reader still accepts only that fixed window and the two bounded
cookie-register reads. This change performs no additional target read.

Semantic RAM parsing failure does not discard an otherwise coherent raw payload.
The original failure status and reason remain, with `Parsed=false` and no parsed
bank or preload list. A coherent SIII snapshot can likewise remain diagnostic
when its report records a duplicate host-info TLV. Its reported SMEM base and
length cannot widen the actual fixed read window.

DXE verifies the complete HOB CRC, exact version and size, serialized lengths,
raw CRCs, zero unused bytes, boolean/reserved fields, fixed address spans and the
relationship between raw data and collector metadata. It checks the descriptor's
serialized TLV boundaries and report fields as diagnostic integrity only. It
then freezes its own copy. `PianoProductSmemReemit`, already bound to the standard
fastboot `BeforeRamlog` callback, re-emits every retained raw byte in six-word
records, with exact offset and byte count. The final shorter record is zero-padded
only for printing; its byte count identifies the original data. No HOB traversal,
SMEM read, cookie read, native call, mapping change or service call occurs on replay.

The records are `PIANO_PRODUCT_EARLY_RAW`, `PIANO_PRODUCT_EARLY_RAM402_RAW` and
`PIANO_PRODUCT_EARLY_SIII_RAW`, followed by the original diagnostic statuses.
This lets a later ramlog regenerate raw evidence after the early print prefix has
wrapped. It preserves the bytes needed to investigate the observed RAMv3
`RamRange` rejection; it does not guess those fields, change the RAM parser,
publish high DDR, create a reservation ledger or grant any memory/DMA permission.
`MemoryOwnershipGranted` and `HighDdrPublished` remain false.

Validation uses the actual SEC observer, actual RAM/SIII collectors, serialized
GUID HOB and actual product DXE consumer. Fifty integrated ASAN/UBSAN cases cover
semantic failure with coherent raw data, first/second partial reads, drift,
cookie failure or change, fixed-window rejection, 8192/2048-byte maxima,
length/CRC/metadata/descriptor/HOB tampering and replay after erasing the fixture
HOB, physical-source bytes and previous raw log. Each replay reconstructs all
original bytes without increasing target-load, BootServices, AT or GCD counters.
The first PHIT/MemoryPeim test also exercises the real publisher and unchanged low
memory path. ARM64 compilation checks the producer and consumer; a compile-time
assertion requires the complete GUID HOB to fit below 64 KiB.

```sh
python3 -m unittest discover -s tests -p test_early_memory.py -v
python3 -m unittest discover -s tests -p test_product_smem.py -v
```

These host fixtures validate retention and integrity boundaries. The revision 2
snapshot has not yet been collected from the tablet; the next product build and
physical ramlog capture remain Root's work.
