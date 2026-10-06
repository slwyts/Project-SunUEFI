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
| Piano_Display_CESTA | five native named tuples AF27000..AF2A000 | 3000 | 0 |

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
64-bit ends,page alignment,names and the exact ROM/source resource bounds.
It refuses source SHA drift,unknown tokens,changed regs,existing row collisions,
owner collisions and any extra or duplicate generated block. All49 native rows
remain semantically and textually preserved; framebuffer cache/size and all DDR
rows are unchanged. No Conventional or System RAM row is added.

Public host APIs are:

```python
original_native(root)  # exact49, or strictly recovered exact52/53
prepare(root, input_native_text, owners=())  # expanded_text, record
verify(root, expanded_text, record)         # exact reconstruction, True
source_files(root)                          # tool + actual pinned input Paths
```

`verify` requires current53 and reconstructs the original49 from the sole exact generated block, requires
its original SHA04ef5a00cee0123d4870a82670e677bfec2af5c360342fc5e0d9092bd7cbb9df,
then re-reads and verifies every actual source pin and rebuilds53 byte for byte.
It does not use a mutable staging table as another canonical source. The record
contains exact windows,raw/page lengths,padding,source hashes,generator hash and
the optional owner snapshot. An empty owner list is not an ownership proof.
After binding replaces the target with53 rows,original_native still accepts only
the exact reversible current transform or the exact historical52 transform;
altered53,52 or49 input is refused.

The actual Mu MemoryPeim/AddHob and PrePiHobLib test produces one MMIO resource
HOB with UC capability for each window and no allocation HOB. Actual ArmMmu
helper code converts DEVICE to device MAIR index plus EL1 PXN/UXN. Actual GCD
resource-attribute conversion yields MMIO/UC capability, and actual ArmCpuDxe
leaf conversion yields UC+XP. These source/host results are expected inputs to
GCD synchronization; they do not assert a live GCD Attributes value or live PTE.
The ARM64 candidate source also compiles with the real headers.

The three tests additionally cover a small copied-file fixture,all source-pin
mutations,49/52/53 reconstruction,record tampering,exact DT reg drift,kernel-resource
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

The CESTA role is independently pinned from original native
`private/analysis/xbl_config_a-0x8358.dtb`,142005 bytes,SHA256
`634ec73dc6d69a07b5af8246e03b0d2ae84dfb9ce9901135121c220443c9cd10`,
and the complete native Clock PE SHA256
`f9e85aa758932b4366c55ec83b58e0176f58fba2944cc2fdd34875a46fdb769e`.
Native `/soc/cesta@af27000` has SDE_CRMB AF27000/400,SDE_CRMB_PT AF27400/400,
SDE_CRMC AF27800/2000,SDE_CRMV AF29800/700 and SDE_CRM_COMMON AF29F00/100.
Their exact ordered union has no hole and is AF27000/3000. Android's independent
CRM tuples and AF27800 syscon corroborate it. AF27D6C lies at CRMC+56C; it is not
the DISPCC row or the RSC AF20000 role. This change does not add CRM base
AF21000/6000 or the RSC window.

Current output is53 rows. Strict recovery accepts either historical exact52 or
current exact53 and requires reconstruction of the same original49; current
verification requires53. Fixtures cover49/52/53 recovery and native CESTA tuple,
name,pin and owner refusal. Four actual MMIO resource HOBs and actual DEVICE/GCD
conversion paths pass the same host checks.

The native Clock's existing initialization performs real vote writes, including
the observed CRMC write. Its existing native-init policy is separate from this
static mapping contract. The mapping tool still authorizes no newly invented
register read or write, and a single FAR is not its range source.
