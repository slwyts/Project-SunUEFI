# Actual ADSP/audio/FastRPC and PCIe DMA consumers

`piano-linux-dsp-pcie-dma.dtso` repairs exactly seven DSP `iommus` properties and
the PCI host `iommu-map`. Kernel sources and the existing stable rootfs are not
changed by this unit. Services use ordinary remoteproc, OF, platform/PCI, IOMMU
and DMA APIs. No USB ancestor context or userspace register programming is used.

The current ROM capture is
`private/analysis/android-memory-2026-10-05/live.dtb`, SHA256
`8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`. Its six FastRPC
consumers provide thirteen pairs. Its audio-ion provides the two pairs carried
by the public GPR dais. All SID/mask values and the DSP address-tagging ABI stay
unchanged; only the IOMMU provider is retargeted to Linux `apps_smmu_ml`.

| Consumer suffix under `/soc/remoteproc-adsp@03000000/glink-edge` | SID/mask pairs | Actual allocation device |
| --- | --- | --- |
| `qcom,gpr/service@3/dais` |1001/80,1041/20| q6apm-dai platform device |
| `qcom,fastrpc/compute-cb@1` |1003/80,1043/20| FastRPC session3 platform device |
| `qcom,fastrpc/compute-cb@2` |1004/80,1044/20| FastRPC session4 platform device |
| `qcom,fastrpc/compute-cb@3` |1005/80,1045/20| FastRPC session5 platform device |
| `qcom,fastrpc/compute-cb@4` |1006/80,1046/20| FastRPC session6 platform device |
| `qcom,fastrpc/compute-cb@5` |1007/40,1067/0,1087/0| FastRPC session7 platform device |
| `qcom,fastrpc/compute-cb@6` |1008/80,1048/20| FastRPC session8 platform device |

Selected352508/d421 kernel source proves their actual creation and DMA use:

* `sound/soc/qcom/qdsp6/q6apm.c:895` calls `of_platform_populate`. The dais is a
  platform DMA consumer although its parent is on the APR bus.
* `q6apm-dai.c` allocates buffers through that device, reads the first SID's low
  bits and prepends the SID to the DMA address sent to the DSP. Provider
  retargeting preserves every SID value and this address ABI.
* `drivers/misc/fastrpc.c:2475` populates platform compute-cb children from RPMsg.
  The CB driver `qcom,fastrpc-cb` sets a32-bit DMA mask and records its real
  platform device; coherent allocations and mappings use it. ADSP's SID tag
  position remains32. No synthetic platform device is introduced.
* `platform_dma_configure` and `iommu_init_device` parse those OF bindings,
  create fwspec and normally probe/attach the ARM-SMMU domain. Existing getters
  can be reused directly, including on PCI devices.

Generated child names depend on `of_device_make_bus_id` and the actual parent
buses. The verifier scans at most4096 platform entries, resolves `of_node` to the
exact expected path, requires one match and checks bound `q6apm-dai` or
`qcom,fastrpc-cb`. It records the observed name instead of inventing one or using
the parent's DMA state as proof.

The ROM maps PCI RID0→SID1400 and RID100→SID1401 in two legacy four-cell entries.
Selected `sm8750.dtsi` expresses the same mapping as two five-cell entries with
mask0/length1. `drivers/of/base.c`, `of_iommu.c` and `pcie-qcom.c` consume the real
provider-sized layout; these files are hash-pinned by the fold tool. The actual
replacement is:

```
<0     &apps_smmu_ml 0x1400 0 1>
<0x100 &apps_smmu_ml 0x1401 0 1>
```

The Wi-Fi endpoint is selected PCI domain0, bus1/devfn0:
`/sys/bus/pci/devices/0000:01:00.0`. Its OF node must be
`/soc/pcie@1c00000/pcie0_rp/wifi@0`, vendor/device17cb/110e, and its OF reg and
host map must match. The real consumer getter must show SID1401/mask0. The old
broad1400/mask7f USB-cloned route is never generated. A host controller or other
PCI device is not accepted as endpoint evidence.

This verifies kernel attachment, declared RID mapping and SMMU configuration.
It does not measure the PARF BDF→SID hardware table or a PCI transfer. Reports
retain `pci_parf_hardware_table_verified=false`, `dma_transfer_verified=false`.
No raw PARF read/write interface is added.

The shared parser `tools/check_piano_kernel_contexts.py` has SHA256
`ae2c661a7a4eec1a84f5720041550f9cadd5884fd567dbfc3d4211d0be7393fc`.
DSP/PCI use their normally attached domain; this unit does not force identity.
It first reads `piano_dma_context`; only exact EAGAIN permits reading the actual
identity `piano_dma_route`. EIO, EPERM, ESTALE, EOPNOTSUPP and missing attributes
fail. Accepted data is read twice and validated against the real provider and
exact stream pairs. Bound identities and firmware-adopted origins remain
explicit; none of these observations grants a memory lease.

Service ordering is based on real creation dependencies:

* ADSP reaches remoteproc `running`, explicitly loads FastRPC, then requires all
  six consumer proofs/thirteen pairs before its power/application handoff.
* Audio loads APM/frontend modules, checks the real dais/two pairs, then
  registers the sound card and PCM buffers.
* Radio loads normal power/PHY drivers to enumerate the actual endpoint, checks
  it, then loads MHI/ath12k. Corrected clock consumers use normal PHY APIs.

Bounded waits up to30 seconds retry ENOENT/ENODEV/EAGAIN during normal creation
or contention. Faults/changed configurations are not retried; no fault is
cleared and no global ready marker is written. The all-scope collector requires
all real consumers to exist and pass, but still does not claim functional DMA.

Independent actual fold output:

```
private/analysis/piano-linux-managed-dsp-pcie-v1/Piano-full-linux-managed-dsp-pcie.dtb
1209191 bytes
SHA256 c2cb041e2b286713c225af7bf0e3a5f7eec8926db168149984823c25ad3f38c4
```

It folds over the corrected-clock candidate SHA256
`20f067af06eb35f7b6fee197c170d06f65e24a418ec5c9166cb5f4d8a3518651`. The real
cpp/dtc/fdtoverlay/libfdt pipeline rejects changes outside eight properties and
requires unchanged tree/phandles/reservations. It checks ROM and real source
hashes before producing a new output directory.

```sh
python3 tools/apply_piano_dsp_pcie_masters.py \
  --base private/analysis/piano-linux-managed-clocks-v1/Piano-full-linux-managed-clocks.dtb \
  --base-sha256 20f067af06eb35f7b6fee197c170d06f65e24a418ec5c9166cb5f4d8a3518651 \
  --output-dir private/analysis/piano-linux-managed-dsp-pcie-NEW

python3 tools/stage_piano_ram_hardware.py \
  --output-dir build/piano-runtime/ram-hardware-adapters-NEW
```

The second command only generates reviewable adapters. v1/v2 remain historical
immutable outputs, not separate final feature profiles; the single canonical
entry/stager now carries the complete scope.

Validation uses seven new actual-source tests in `tests/test_piano_dsp_pcie.py`,
the four existing context tests and ten adapter tests. They exercise the real
fold, dynamic discovery, all fifteen pairs, actual PCI identity/map, exact
EAGAIN fallback, immediate failure, creation timing and actual script order.
Fixtures are not physical-device results. No current rootfs or tablet is touched.
