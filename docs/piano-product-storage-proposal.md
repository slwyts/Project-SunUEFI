# Product storage proposal: one reserved UFS container

The current device is **not provisioned**. The former 14MiB restore-test gap is
not a permanent reservation. No device write, GPT change or formatter has been
executed by this work. Capture5 independently reread all nine original objects
under Android and matched capture1: both GPT headers/arrays, MBR, the whole zero
gap, first block and two neighbors. There are still 79 active LUN4 partitions.

`tools/prepare_product_storage.py` creates a private, PC-only proposal from the
fixed checked capture. It has no device executor or authorization switch. It
uses actual C `PianoNvFvbBuildBlank` and `PianoNvJournalCommit` through
`emit_nv_seed.py` to produce the standard empty NV/FTW snapshot and validates its
own journal packing byte for byte against that C output.

The current complete proposal is under
`private/provisioning/piano-storage-v1-final/`. Its 14MiB container SHA256 is
`a1428cd379314b7a665f37ff471c050d5f877648c0aaa5b662ff2d26e660aa2f`;
manifest SHA256 is
`03745f229b8971d845c0eb0f9cbbcf474af61c5476d59eb4c20cb83f208a531c`.
These are proposal bytes, not evidence that storage has been written or accepted.

## Exact proposed change

Only unused GPT entry95 would become `PianoUEFI Storage`, with type GUID
`3e4ea305-5b3d-49a4-b3b4-5844ef513648` and NO_BLOCK_IO attribute2. Its unique
volume UUID binds the entry to two immutable container headers. Existing GPT
entries remain byte-identical; header changes are limited to header CRC and
entry-array CRC. MBR is unchanged. Primary and backup GPT need consistent
updates; the PC proposal contains both complete checked copies.

The container is fixed to LUN4, 4096-byte sectors, physical LBA375040..378623
inclusive. Relative block allocation is:

| Relative blocks | Purpose | Size |
| --- | --- | --- |
| 0..1 | Two identical immutable volume headers | 8KiB |
| 2..2047 | Public bounded FAT12 child volume | 8MiB minus 8KiB |
| 2048..2815 | Private NV journal A | 3MiB |
| 2816..3583 | Private NV journal B | 3MiB |

The public child excludes container headers and both NV slots. Original LUN and
partition handles remain read-only. The 14MiB allocation is for firmware state
and small maintenance files; it cannot hold a complete Linux distribution.

Both volume headers bind layout1, original disk GUID, volume UUID, type GUID,
LUN, sector size, exact bounds and GPT index. Their whole-block CRC covers all
4096 bytes. The product writer requires the actual reservation and both headers
to agree with fresh geometry, write-protection and both GPT copies. A boolean
or host manifest does not enable writes. Current original GPT/no reservation
must return NOT_FOUND without WRITE or SYNC.

## Initial NV contents and persistence boundary

The standard C seed is 576KiB: variable region256KiB, FTW workspace64KiB and
spare256KiB. The variable FV/header uses the standard authenticated variable
format. Workspace/spare are erased FF for the standard FTW driver to initialize;
the producer does not fabricate FTW transaction records. Journal A starts at
sequence1, with header at block0, payload at blocks1..144 and committed footer
at block767. Journal B has no commit. The blank snapshot SHA256 is
`3b4b4edbb624ca378abf884340a744673b23c7a4c4ee933426bde1f7f2028cdd`.

Future updates invalidate only the inactive slot footer, write and verify the
new full snapshot, then commit and verify its footer last. The old valid slot
remains a recovery candidate. Host fault-injection tests do not prove physical
power-loss behavior; durable UFS acceptance still needs explicit testing.

Standard VariableRuntimeDxe/FTW initialization ordering remains a real issue:
native HALIOMMU depends on VariableWriteArch, while persistent variable restore
needs that storage path. The current product cannot claim persistent variables
by attaching a late FVB to an already initialized emulated cache. Runtime UFS
ownership and durable runtime SetVariable also remain incomplete. Boot-only
storage code must not run after ExitBootServices.

## Review before any real provisioning

Permanent reservation and GPT updates are a different operation from the
previously authorized write/readback/restore test. The complete proposal can be
reviewed now; device execution requires explicit approval of this exact scope,
fresh original GPT/gap checks and a tested provisioning/rollback executor. No
approval has been received, and this producer cannot apply the proposal.

```sh
# PC-only; output must be a new directory under private.
python3 tools/prepare_product_storage.py --output private/provisioning/NEW_NAME
python3 -m unittest discover -s tests -p test_prepare_product_storage.py -v
```
