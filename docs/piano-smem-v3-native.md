# Native RAMv3 observation model

The captured EnvDxeEnhanced PE is 77824 bytes, SHA256
`593d9e766c0070e01d4c6684b7bb8050f32f77ea3ca300c6a110ad3903de17e3`.
Its section raw offsets equal their RVAs. Interface RVA102E0 contains revision
10002 and six methods:2268,228C,22F4,22A8,23D4,2420. The older QcomPkg header
has five methods and an incompatible preloaded ABI despite the same revision.

The native initializer reads item402 through8638 and selects the legacy64-byte
path only for version1 at8B58/8B5C. The observed version3 follows the72-byte
path. At8BA8 its cursor is payload+30, equal to entry+18. The relevant accesses
and filters are:

| RVA | Verified native operation | Observation meaning |
|---|---|---|
| 8BC0/8BC8 | read entry+2C; require type1 | Current RAM record selection |
| 8BC4 | read entry+10 | Base |
| 8BD0/8BD4 | read entry+24; require category14 | Current RAM record selection |
| 8BE8 | read entry+40 | Native current length |
| 8C14 | store Base/current in cached+20/+28 | No raw-length or zero-current gate |
| 8C3C/8C40 | unsigned type-5<=4 | Preloaded types5..9 |
| 8C6C | read entry+18 | Preloaded size |
| 8CCC | advance48 | Next72-byte record |
| 233C/234C | load/store32-bit count | GetRAM public count width |
| 23AC..23BC | copy cached Base/current; stride16 | GetRAM public record |
| 2450/2484 | load/store64-bit count | GetPreloaded public count width |
| 24CC..24EC | copy Base/size/type; stride24 | Native preloaded record |
| 8738 | sum entry+18, stride72, no category/type filter | Total-like field sum; not usable DDR |

The native GetRAM helper also returns success for an empty cached list at885C.
GetVersion2268 writes2/0 independently of RAM402's version. Initialization has a
NOT_READY fallback at8B00..8B48 that creates80000000/60000000, so a protocol
revision, success status or getter result alone is not source or ownership proof.

Test95's physical RAM payload is2328 bytes, CRC7C271814, SHA256
`16aed6a815309af4109f34000f43de0058d310e293b2aecc29c6d4d9a7ed828c`.
It has15 used records in a32-record payload; the17 unused records are zero.
The SIII snapshot is20 bytes, CRC2776A43D, SHA256
`313b218fd801ca2d9cba0450e8e4b97c046ef77443e24909c0d66234959053bc`,
and reports81D00000/200000,675 items and no TLVs.

Applying the native filter to those captured bytes selects12 current records,
11 positive and one empty. Their positive current spans do not overlap. Current
record12 (8B5700000/1E00000) lies inside record6's nonzero declared container;
record13 (82600000/55A00000) lies inside record2; record14 (81200000/40000) lies
inside record0. Those three records have zero entry+18 length. Record3 retains
its declared B120000 length and a zero current length. The observed relationships
support separate declared-container and current-slice views; they do not prove
who created the slices or owns their excluded gaps.

The category14/type2 record10 at D8000000/40000 is excluded from both native
current and preloaded filters. Its specific reservation owner and meaning remain
unknown. Categories4/5 are likewise outside this RAM view. No unknown type is
silently relabeled as a bank, preload or authorized reservation. The total-like
getter's field sum is40026C000, while the selected declared-container fields sum
to400000000 and positive current fields sum to3EA85D000. None is an allocation
or memory-map permission.

The pure parser now keeps the strict version1/version2 model unchanged and uses
the proven current view only for exact version3. Its existing Banks array holds
native category14/type1 current records, including zero current lengths. RawSize
preserves entry+18 independently; it neither bounds nor authorizes the current
span. Positive current ends must not overflow and positive current spans must
not overlap. Native type5..9 preloads still use nonzero, bounded raw sizes. The
existing OtherCategoryCount includes ignored/unknown v3 types, and every original
record remains in the coherent raw HOB and the analysis script's full record view.
Future versions remain unsupported. An empty selected v3 view is valid observation,
as in the native getter, without establishing any usable memory.

The DXE consumer re-parses its already CRC-checked local version3 raw bytes with
this same pure model and compares all parsed counts and records to the serialized
report. Version1/version2 consumer gates remain unchanged. The report struct,
revision2 HOB format/size and cold observer are unchanged. Parsed is diagnostic
structure success; memory ownership and high-DDR publication remain false.

```sh
python3 tools/analyze_native_ram_v3.py
python3 tools/analyze_native_ram_v3.py --disassemble
python3 -m unittest discover -s tests/unit -p test_smem_ram.py -v
python3 -m unittest discover -s tests/unit -p test_product_smem.py -v
python3 -m unittest discover -s tests/unit -p test_early_memory.py -v
```

The script hashes all three evidence files, validates the vtable and selected
machine instructions, and decodes only captured CPU bytes. It executes no native
getter and writes no file or device. Host tests run actual C under ASAN/UBSAN,
including the SHA/CRC-verified test95 bytes independently in the parser and in the
actual SEC-to-HOB-to-DXE pipeline. The adapted firmware has not yet been booted.
