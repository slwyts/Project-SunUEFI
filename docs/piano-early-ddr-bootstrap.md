# Early DDR bootstrap evidence and remaining binding

The first platform memory query runs in SEC `InitializeMemory`, before
MemoryPeim creates resource/allocation HOBs and configures the MMU. The native
Env RAM protocol is produced in DXE. Its later inventory cannot authorize
re-running the first memory initialization or changing a live DXE map.

The current BootShim record at `0xA7FFF000` preserves its magic, original x0/DTB
at +8 and CurrentEL at +16. It does not preserve SMEM descriptors, RAM item402
or preloaded-image data. The handoff DT is therefore available as a potential
early source, but it is not a complete live ownership ledger.

The native product MemoryMapLib, pinned Linux `sm8750.dtsi` and Android's
`81d00000.smem_region/of_node/reg` agree on the fixed SMEM window
`0x81D00000..0x81F00000` (2MiB). Android has no confirmed safe item402 export;
no physical-memory scan or SMEM payload read was performed by this research.

The captured EnvDxeEnhanced path at RVA `0xBF00` reads the two 32-bit TCSR
cookies at `0x1FD4000` and `0x1FD4004`, forms a descriptor pointer and checks
`SIII` (`0x49494953`). Its descriptor uses size32 at +4, base64 at +8,
itemCount16 at +16, TLVCount16 at +18 and TLVs at +20, including tag `0x4853`.
The actual cookie values, pointer bounds and descriptor contents remain unknown.
They must not be converted into arbitrary address-read permission.

The native RAM path at RVA `0x8638 -> 0xA868` requests SMEM item402 (`0x192`).
The actual payload parser must validate both magic words
`0x9DA5E0A8`/`0xAF9EC4E2`, version32 at +8, count32 at +0x10 and entries at +0x18.

| Supported observed format | Entry stride | Relevant fields |
| --- | ---: | --- |
| v1 | 64 | Base64 +0x10, Size64 +0x18, Category32 +0x24, RawType32 +0x2C |
| v2 | 72 | Same fields, AvailableLength64 +0x40 |

Category14 denotes SDRAM; type1 is the available-bank path. The native preloaded
path uses types5..8 for v1 and5..9 for v2. No actual live payload version has been
obtained, so unsupported versions must remain unsupported. The native
1.5GiB fallback `0x80000000..0xE0000000` is not full-DDR evidence.

The [official Linux SMEM implementation](https://github.com/torvalds/linux/blob/v6.16/drivers/soc/qcom/smem.c)
defines the distinct major11 global TOC and major12 partitioned structures.
The SMEM version word is at +0x5C. For major11, item402's TOC entry is at
`0x81D019F0`; allocated, offset, size and auxiliary-region fields require bounds
checks. Major12 instead requires the final-page `$TOC` at `0x81EFF000`, bounded
partition entries, `$PRT` headers and the actual item headers. Reading major12
through the major11 item offset would be incorrect.

The next source unit is a bounded readonly collector with a recoverable TryRead
binding, fixed-window limits, strict item/version/count parsing and repeated
snapshot coherence checks. It must not call native initialization, acquire its
remote locks, allocate, modify SMEM, publish a memory map or grant ownership.
An unbound TryRead adapter, unsupported locator or inaccessible cookie remains
an explicit failure. Static producer-lifetime scratch storage is required
rather than placing two complete payload snapshots on the early SEC stack.

Full publication still needs actual early cookie/item402 evidence, all DDR bank
and preloaded records, fixed/dynamic DT reservation accounting, live firmware
owners, cache attributes and a trusted cold-phase binding before MemoryPeim.
Linux ARM64 EFI boot uses the EFI memory map as its RAM authority, as described
in the [official ARM UEFI documentation](https://github.com/torvalds/linux/blob/master/Documentation/arch/arm/uefi.rst).
Neither an Android RAM count nor a successful raw handoff replaces that proof.
