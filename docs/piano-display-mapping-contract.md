# Pinned display MMIO mapping candidate

`tools/piano_display_mapping.py` prepares an independent, host-validated typed
MemoryMapLib candidate. It does not call prepare, edit the current product table,
access a device, map live memory or read/write hardware registers. Root owns any
later product binding and physical acceptance.

| Name | ROM register span | 4KiB mapping length | Padding |
|---|---|---:|---:|
| Piano_Display_GCC | 100000 / 1F4200 | 1F5000 | E00 |
| Piano_Display_DPU | AE00000 / 93800 | 94000 | 800 |
| Piano_Display_DISPCC | AF00000 / 20000 | 20000 | 0 |

The ROM input is the exact original captured live.dtb,1110810 bytes,SHA256
`a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7`.
The GCC/clock and DISPCC reg tuples agree with the pinned kernel sm8750.dtsi.
The kernel splits DPU into MDSS AE00000/1000 and MDP AE01000/93000; their envelope
ends at AE94000 and matches the ROM mdp_phys page envelope. The original800-byte
tail difference remains visible in metadata. Driver sources use qcom_cc_map and
msm_ioremap with the corresponding resources. Pins identify actual bytes, without
claiming a particular kernel commit is the current HEAD.

Each new row is actual AddDev/MMAP_IO/EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE/
EfiMemoryMappedIO/ARM_MEMORY_REGION_ATTRIBUTE_DEVICE. Typed C compilation checks
the pinned real enums rather than trusting symbolic strings: AddDev2,MMAP_IO1,
MMIO11,resourceUC400,DEVICE6. The generator validates every existing native row,
including only compatible device-containment aliases, all explicit owner spans,
64-bit ends,page alignment,names and the exact three ROM/source resource bounds.
It refuses source SHA drift,unknown tokens,changed regs,existing row collisions,
owner collisions and any extra or duplicate generated block. All49 native rows
remain semantically and textually preserved; framebuffer cache/size and all DDR
rows are unchanged. No Conventional or System RAM row is added.

Public host APIs are:

```python
original_native(root)  # exact49, or strictly recovered/verified exact52
prepare(root, input_native_text, owners=())  # expanded_text, record
verify(root, expanded_text, record)         # exact reconstruction, True
source_files(root)                          # tool + actual pinned input Paths
```

`verify` reconstructs the original49 from the sole exact generated block, requires
its original SHA04ef5a00cee0123d4870a82670e677bfec2af5c360342fc5e0d9092bd7cbb9df,
then re-reads and verifies every actual source pin and rebuilds52 byte for byte.
It does not use a mutable staging table as another canonical source. The record
contains exact windows,raw/page lengths,padding,source hashes,generator hash and
the optional owner snapshot. An empty owner list is not an ownership proof.
After binding replaces the target with52 rows,original_native still accepts only
the exact reversible transform; altered52 or49 input is refused.

The actual Mu MemoryPeim/AddHob and PrePiHobLib test produces one MMIO resource
HOB with UC capability for each window and no allocation HOB. Actual ArmMmu
helper code converts DEVICE to device MAIR index plus EL1 PXN/UXN. Actual GCD
resource-attribute conversion yields MMIO/UC capability, and actual ArmCpuDxe
leaf conversion yields UC+XP. These source/host results are expected inputs to
GCD synchronization; they do not assert a live GCD Attributes value or live PTE.
The ARM64 candidate source also compiles with the real headers.

The three tests additionally cover a small copied-file fixture,all source-pin
mutations,49/52 reconstruction,record tampering,exact DT reg drift,kernel-resource
drift,owner tail-byte collisions,overflow and malformed owners. They run actual
C under ASAN/UBSAN and do not invoke an allocator or MMU on the tablet.

```sh
python3 -m unittest discover -s tests -p test_piano_display_mapping.py -v
python3 tools/piano_display_mapping.py --output-dir artifacts/display-mapping-candidate
```

The exported candidate remains
`HOST_DISPLAY_MMIO_CANDIDATE_REQUIRES_LIVE_AT_GCD`,hardware_verified=false and
register_access_authorized=false. Root must verify actual AT identity/cache,
GCD type/Attributes and the guarded reader's exact allowlisted registers after
binding. Resource UC capability is not current GCD Attributes. Mapped padding
is not a readable-register whitelist. In particular the pinned DISPCC driver
limits max_register to F004 and notes10000/10004 may be TZ: the larger20000 DT
window cannot authorize arbitrary reads. There is no cold direct register probe,
GOP/FB rewrite,high-DDR change or hardware-ready claim in this module.
