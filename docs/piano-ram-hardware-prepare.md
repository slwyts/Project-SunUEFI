# Piano RAM hardware preparation and reference-clock bindings

The canonical entry is
`linux/userspace/piano-ram-hardware-prepare`. The RAM bootstrap calls
it before handing PID 1 to Debian systemd. It verifies an explicit `piano.root=ram`
command line, actual RAM-backed root, the current full module directory, normal
clock-provider binding, then normal `modprobe arm_smmu` and fresh USB/QUP route
readbacks. It does not load the UFS driver or open Android block devices.

The reviewed `tools/check_piano_kernel_dma_routes.py` is installed **unchanged** as
`/usr/lib/piano/piano_dma_routes.py`, SHA256
`1f2c26329c00b5b791a101d409032cd3d8b1962f83018937807c5cbbb7f5a2e1`.
The runtime imports this exact parser and resolves actual `of_node` links before
reading each kernel attribute twice. It does not implement another SMMU parser,
clone USB's context bank, program stream matches or access raw registers.

The second unchanged shared parser, `tools/check_piano_kernel_contexts.py`, is
installed as `/usr/lib/piano/piano_dma_contexts.py`, SHA256
`ae2c661a7a4eec1a84f5720041550f9cadd5884fd567dbfc3d4211d0be7393fc`. It validates the
real selected consumer/provider, fresh kernel-private versus hardware stage1
context configuration, exact SIDs/masks and fault-free register snapshots. This
provides configuration evidence; it does not claim a successful DMA transfer.

`/run/piano-hardware/<scope>.json` records success or the specific refusal. These
files are diagnostics; no service consumes them as authorization. Every service
guard rereads actual sysfs. The entry never writes `/run/piano-smmu-ready`. Common
USB/QUP route success is explicitly not complete hardware readiness, successful
DMA or an authorization to map DDR. Firmware-adopted and kernel-installed routes
retain the distinctions reported by the shared verifier.

## Exact reference-clock repair

`linux/dts/piano-linux-tcsr-clocks.dtso` and
`tools/apply_piano_tcsr_clocks.py` fix a concrete three-source mismatch:

* The current Android capture has `/soc/clock-controller@f204008`,
  `reg=<0xf204008 0x3004>`, UFS qref clock ID1. Capture SHA256 is
  `8056ae623549f4dbefbf3c2615c134fa670cafa83387235575ab573896636fcc`.
* The selected Linux `sm8750.dtsi` uses the same range and an RPMh CXO parent.
  `tcsrcc-sm8750.c` implements PCIe ID0 at offset0 and UFS ID1 at offset0x1000;
  USB2/USB3 IDs2/3 use offsets0x2000/0x3000. Its real platform driver name is
  `tcsr_cc-sm8750`. PHY drivers vote these through normal bulk clock APIs.
* The public overlay instead points UFS/PCIe to fixed clocks and USB2/USB3 to a
  retagged legacy `syscon@1fc0000`. Public comments describe manual `devmem`
  reference-gate writes. Those commands cannot be retained in this RAM entry.

The repair reuses the existing real node and phandle, changes its compatible and
CXO parent, restores the wrong-address syscon's compatible to `syscon`, and
rewires exactly four PHY clock lists. It retains all other list members/IDs.
The fold rejects a changed input SHA, changed selected kernel clock source,
changed current-ROM capture, unexpected preexisting consumer list, or any change
outside these seven properties. Tree nodes, phandles, reservations, iommus,
statuses, GPIOs and register ranges remain byte-identical as properties.

Actual independent output:

```
private/analysis/piano-linux-managed-clocks-v1/Piano-full-linux-managed-clocks.dtb
1209151 bytes
SHA256 20f067af06eb35f7b6fee197c170d06f65e24a418ec5c9166cb5f4d8a3518651
```

It was folded over the six-master Linux-owned DMA candidate, SHA256
`7b96afd614fdddbb8916f9ebdcf3876bfac8af8a6ca21f840ccf1c86af7c18f8`. Neither source
DTB was overwritten. Reproduce into a **new** private output directory:

```sh
python3 tools/apply_piano_tcsr_clocks.py \
  --base private/analysis/piano-linux-owned-dma/Piano-full-linux-owned-dma.dtb \
  --base-sha256 7b96afd614fdddbb8916f9ebdcf3876bfac8af8a6ca21f840ccf1c86af7c18f8 \
  --output-dir private/analysis/piano-linux-managed-clocks-NEW
```

The runtime clock scope validates the actual four DT consumer lists, normal
provider parent, correct range and actual bound driver. It reports this as
**binding evidence**, with `clock_enable_or_rate_readback_verified=false`.
Successful binding is not a measurement of gate state or clock frequency.

## Complete public service chain and remaining master proofs

The pinned public Debian source is
`25babfe3ff5d8ddee98b1e0ea88152d69a0c01b1`. The stager preserves its service units,
enablement links, firmware, every hardware module command and the default enabled
configuration. It creates eight strictly pinned script adapters. Disabled=0
configuration remains the public user's explicit choice. Old ADSP/audio failure
paths that returned zero are changed to nonzero failures.

| Service | Required DMA/clock evidence | Current adapter behavior |
| --- | --- | --- |
| touch and keyboard | QUP1 GPI b6, wrapper a3; QUP2 GPI436, wrapper423; all mask0 | Fresh shared six-master parser; retains touch rebind and keyboard firmware/HID chain |
| native GPU | Adreno GPU SID0/1, GMU5; normal MSM-managed domains | Normal MSM probe and bound-driver wait first, then mandatory GPU+GMU stage1 readback |
| native display | MDSS800/mask2 plus Adreno proof | Nonblocking prebind diagnostic, then mandatory MDSS+GPU+GMU stage1 readback after DPU binds |
| radio | Real WCN7861 RID100→SID1401/mask0 and CLKREF consumer | Normal PHY/host enumeration, fresh endpoint/domain proof, then MHI/ath12k |
| ADSP | Six real FastRPC consumers, thirteen SID/mask pairs | Normal remoteproc running and FastRPC child creation, fresh proofs, then application/power handoff |
| audio | Actual GPR dais1001/80 and1041/20 | Normal APM/frontend creation, fresh dais proof, then card/PCM buffers |
| video | Iris1940 **and1947**, translated DMA domain | Normal group type switch to DMA and actual driver binding, then nonblocking context observation |
| camera | CAMSS1c00 translated DMA domain | Normal group type switch to DMA and actual driver binding, then nonblocking context observation |
| UFS | SID60/mask0 plus real qref binding | Read-only scope is available; bootstrap does not load storage |

The complete merged DT has two Iris SIDs; the public standalone video overlay
shows only1940. The additional DSP/PCI overlay retargets seven real DSP consumers
from the legacy apps SMMU to the real Linux provider. Their stream pairs are:
1003..1008/mask80 plus1043..1048/mask20, except cb5 uses1007/40,1067/0,1087/0.
GPR dais uses1001/80 and1041/20. The ROM and Linux source agree RID0→1400 and
RID100→1401. Linux uses explicit mask0/length1 five-cell tuples for its two-cell
provider; the old broad1400/mask7f cloned route is never generated. The existing kernel
already matches `qcom,sun-adsp-pas` to its SM8750 resources; that compatible name
itself is not a missing driver.

The original six-master identity ABI comes from
`74c517825a95113f35c121db136e3880d44b9806`. The independent `piano-smmu-context`
worktree adds the real ADMIN-only `piano_dma_context` getter for both normal DMA
and MSM-managed stage1 domains. Identity and translated domains use distinct
interfaces. DSP/PCI scopes accept the actual normally attached domain without
forcing it. Only exact stage1 getter EAGAIN permits identity fallback. Faults,
stale hardware, denied access and missing ABI files fail. `--require-scope all`
now requires every actual consumer proof; even success still reports
`dma_transfer_verified=false` and `full_hardware_ready=false`.

Guard placement follows actual Linux ownership acquisition. Video/camera must
first write `DMA` to their normal IOMMU group `type` interface. That operation
allocates/attaches the kernel domain and refuses a switch while a driver owns
the group. Module loading and actual driver binding remain mandatory. The
additional `piano_dma_context` snapshot follows binding as a nonblocking
observation: an unavailable readback is recorded with its error, not treated
as proof that normal Linux initialization must stop. GPU/GMU domains are created by
`msm_iommu_new` through paging-domain allocation and `iommu_attach_device` during
normal MSM/Adreno initialization, so their guard follows `modprobe msm` and the
bound-driver wait. `msm_kms_init_vm` similarly attaches the MDSS display domain
when DPU initializes. Its mandatory guard follows the DPU bound-driver wait.

Before DPU takeover the old display context may legitimately still have M=1.
`--observe-scope mdss-prebind` records even an unavailable identity observation
and returns zero to permit normal driver probe. Every such report has
`observation_only=true`, `full_hardware_ready=false`; it never grants readiness.
`--require-scope display-active` remains mandatory after DPU binds and requires
real MDSS, GPU **and** GMU stage1 evidence. The observation mode also supports
camera/video after their normal drivers bind; failure reports use
`OBSERVATION_UNAVAILABLE` and never claim device readiness.
Historical `ram-hardware-adapters-v1` output remains unchanged;
new context-enabled adapters must be generated into a new directory.

GPR/FastRPC children only exist after normal remoteproc and module startup.
The verifier discovers the unique real platform child by its exact OF path and
checks its bound driver, without inventing a generated bus name. ADSP proof
follows running/FastRPC creation; audio proof follows frontend creation and
precedes PCM/card allocation; PCI proof follows normal PHY enumeration and
precedes MHI/ath12k. Bounded waits only retry normal missing/busy creation states,
not faults or changed configuration. See `docs/piano-dsp-pcie-dma.md` for actual
source paths, all fifteen DSP pairs and precise PCI mapping evidence.

## Canonical generation and staging

Generate reviewable adapters without modifying a distro tree:

```sh
python3 tools/stage_piano_ram_hardware.py \
  --output-dir build/piano-runtime/ram-hardware-adapters-NEW
```

The output contains the canonical entry, unchanged verifier, eight adapters and
their input/output hashes. The default performs no service execution. Optional
staging adds `--rootfs build/distros/EXPLICIT-DERIVED-TREE`; the target must already
contain the complete public service overlay. Every destination is preflighted to
reject guest symlink traversal. Original imported artifacts and public sources
are never accepted as staging targets. This implementation has **not** been
staged into the existing GNOME rootfs or run on the tablet.

Validation:

```sh
python3 -m unittest discover -s tests/unit -p test_piano_ram_hardware.py -v
```

Ten tests execute the actual runtime parser and normal-load sequencing against
explicit sysfs fixtures, regenerate all pinned public adapters and the real
cpp/dtc/fdtoverlay/libfdt clock repair, reject changed/faulted routes and stale
markers, reject dummy/unbound clocks before module calls, retain every public
module command, stage into a temporary complete tree while preserving all units
and enabled configuration, verify normal domain-switch/probe/guard ordering,
consume the unchanged real context parser, require both Iris streams and GMU,
reject wrong domains/faults, verify the actual main nonblocking prebind failure
path, refuse unsupported domains and preflight guest-symlink escapes.
No test reads host MMIO, loads a host module or accesses the tablet.
