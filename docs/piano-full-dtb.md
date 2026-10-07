# Complete Piano DTB folding candidate

This is a host-built **full functional graph candidate**, not a hardware result.
The camera top-level overlay includes CPU frequency, keyboard/touchpad, video,
audio/ADSP, display/GPU, UFS, WLAN/BT, touch, USB and providers. Published enabled
nodes remain enabled; the builder does not disable features to suppress schema
or resource diagnostics. No device, module loading, partition write or MMIO
operation is performed.

## Pinned input layers

The current builder pins `upstream/debian-piano-current` at
`25babfe3ff5d8ddee98b1e0ea88152d69a0c01b1`, top-level
`boot/dtbo-piano-camera.dts`, and explicit headers from the complete kernel
candidate `build/kernel-worktrees/piano-full-integration` at
`352508459733d3e6d349ea5581a8dd2fd8bb4180`. Source commits, tracked cleanliness,
all preprocessing input hashes and raw DT input hashes are checked and recorded.
The earlier e400 overlay and disabled display-only topic are not these inputs.
This source update changes camera userspace startup only; the DT overlay is
unchanged. Existing fd6266 output directories retain their original provenance.

The selected current-ROM layer is vendor_boot_a **DTB index4**, extracted at
`0x21B5430`, plus official DTBO **index0**. The actual current live FDT is the
frozen `private/analysis/android-board-runtime-2026-10-05/live.dtb`. Live already
contains stock overlay and ABL corrections; it must not receive the complete
published stock-containing chain a second time.

The complete published overlay is compiled once per build with explicit kernel
include path. Real `dtc -@`, `fdtoverlay`, `libfdt fdt_check_full` and DTB
roundtrip checks are used. The full chain is folded once onto selected ROM base;
a separately folded official stock0 gives the current-ROM comparison layer.
The already compiled overlay is also split at the published fragment200 boundary
to observe its embedded stock prefix without recompiling or mutating source.

## Reconciliation rather than blind replay

External and local fixups are canonicalized when comparing stock fragments,
so numeric phandle renumbering is not counted as a physical resource change.
The current published chain and captured official stock0 are not byte-identical:
80 canonical fragments agree, 33 differ and 9 current-ROM fragment paths are
absent from the published embedded prefix. These counts alone do not determine
hardware compatibility.

The tool additionally compares the **actual merged stock-prefix properties**
against the current official ROM layer. Known generic and explicit downstream
reference ABIs are compared/remapped by provider node path; opaque integers are
never guessed to be phandles. Public additional fragment operations are resolved
through actual external/local fixups and target paths. Their functional changes
are retained. Other stale embedded-prefix properties and missing current-ROM
nodes are restored with current-ROM data and reference identities.

Two intentional changes made inside the published stock prefix are bound to
reviewed path/value/source records and retained: USB `dr_mode=peripheral` in
fragment29, and `no-map` on the **unchanged** splash physical range in fragment131.
The second is the published simpledrm WC mapping requirement. A changed audited
value or physical splash range is rejected. This source intent is not evidence
that PHY/USB/display hardware has been validated here.

Next, genuine current-ROM-to-live differences restore actual memory/DDR,
reserved/chosen, SCM, hypervisor, ADSP and codec corrections. Phandle definitions
are not replayed numerically; known references resolve to the folded provider
identity. The live reservation map and boot CPU are retained. Old
`linux,initrd-start/end`, `kaslr-seed` and `rng-seed` are removed. The OS loader
owns fresh initrd/seeds and must select its own command line; a captured Android
`bootargs` string is not a distribution policy.

## Output and review evidence

```bash
python tools/build_piano_full_dtb.py \
  --kernel-tree /home/slwyts/Project-SunUEFI/build/kernel-worktrees/piano-full-integration \
  --output-dir private/analysis/piano-full-dtb-fd6266-final
```

The private output contains `Piano-full-camera.dtb`, compiled `camera.dtbo`,
`manifest.json`, and real compiler/folding/roundtrip logs. Raw DTBs contain
captured boot metadata and are not copied to public artifacts. A different
existing candidate is not silently replaced. Hash/compile/fold failures retain
a failed manifest.

The manifest retains source input hashes, stock fragment and property drift,
per-property reg/IRQ/GPIO/supply/clock/ICC/IOMMU/memory classification, explicit
public intents, current-ROM/live reconciliation records, memory/reservation
differences, actual phandle/provider-cell/supply/endpoint checks, and every new
unresolved graph diagnostic. `empty_property` identifies published empty legacy
properties without pretending that they are valid references. Reconciled stock
references and public functional conversions remain distinguishable.

Published comments/declarations mentioning bypass/devmem/stand-in/dummy/fixed
clock/fixed regulator/driverless dependencies are retained with source path and
line. The actual published `rootfs-init`, `piano-qup-smmu` and `display-start`
prerequisite sources are also hashed and their relevant operations recorded,
with both `executed=false` and `satisfied_in_this_candidate=false`. The complete
public graph still depends on staged runtime sequencing and
some bootloader-held rails/votes, synthetic providers, and direct SMMU/clkref
preparation in the reference environment. None of those dependencies is removed,
silently executed, or counted as satisfied by this tool. Duplicate physical
provider definitions, global SMMU ownership, firmware, graph binding and those
runtime prerequisites require separate kernel/handoff acceptance.

FDT structural and roundtrip checks do not imply all retained vendor properties
conform to mainline schemas. Raw resource errors are mapped to registered driver
tables and reviewed consumption paths. Unclassified or still-consumed blockers
retain `OFFLINE_FOLDED_CANDIDATE_WITH_UNRESOLVED_GRAPH`; a completely classified
legacy list is `OFFLINE_FOLDED_CANDIDATE_WITH_LEGACY_DIAGNOSTICS`. Neither status
is a boot-readiness claim. Diagnostics are not hidden by deleting nodes.
`hardware_verified=false` and
`memory_ownership_authorized=false` apply to every result, including one without
resource errors. Current live RAM/reservations/hypervisor values do not grant
the next UEFI boot permission to reclaim memory or operate active owners.

## Tests

`python -m unittest discover -s tests/unit -p test_build_piano_full_dtb.py -v` uses
actual libfdt, dtc and fdtoverlay. It covers malformed FDT/header rejection,
input pin failure, known-reference bounds, unknown opaque integers, real
overlay folding/canonical stock drift, embedded-prefix extraction, target
resolution, preserved explicit functional bindings, restored old wiring,
memory/reservation retention, path-based phandle remap and removed transient
Android initrd/seeds. It does not substitute a synthetic fixture for the
separately generated complete pinned candidate.

## Driver consumption review and narrow UART correction

The original119 diagnostics are mapped by `tools/piano_dtb_impact.py` against
selected352508 source. Only OF tables referenced by `.of_match_table` count as
registered driver matches; client/implementation helper tables stay separate.
Node and ancestor status use OF availability semantics. This proves source
capability, not runtime module loading or device binding.

| Impact of original119 | Count | Evidence |
| --- | ---: | --- |
| Disabled residual |65| Unavailable node/ancestor, mostly old QUP buses and KGSL |
| Unmatched vendor residual |32| No selected-source registered match for vendor TBU/CVP/CESTA, SDE, CNSS, video, DCVS and other legacy nodes |
| Matched driver, unused property |18| PAS does not obtain ICC paths; GCC/QMP/DWC3/PCIe do not request the listed cleared vendor supply names |
| Consumed optional, assumes on |1| `ufshcd_populate_vreg` treats empty vdd-hba phandle as no regulator and succeeds, assuming it already enabled |
| Consumed dummy fallback dependency |2| M31 consumes vdd/vdda12 with normal bulk regulator get; populated-DT ENODEV fallback supplies dummy regulators, not physical rail control |
| Consumed blocker corrected |1| UART7 needs valid qup-core/config ICC paths in `geni_serial_resource_init -> geni_icc_get` |

The six tuple errors and one unresolved phandle all belong to retained
`qcom,qsmmuv500-tbu`/`qcom,qtb500`, `qcom,msm-cvp,bus` or `qcom,sde-cesta` nodes
without registered driver matches. They combine vendor one-cell ICC references
with a provider changed to mainline two-cell ABI. Their raw diagnostics remain;
no false provider #cells were added and no nodes were deleted.

`linux/dts/piano-uart7-icc.dtso` uses exact same-UART7 ICC paths from
selected `sm8750.dtsi:2014..2028` and its headers. Only the a9c000 debug UART's
interconnects/names are replaced by qup-core/config. Its target compatible and
physical reg are checked; fdtoverlay output must preserve all other properties,
provider cells and statuses byte-for-byte. DDR is optional for this FIFO debug
UART. Frozen kernel/public source/pins remain unchanged. Pins, wire access and
other possible probe dependencies are still not a serial hardware pass.

The corrected candidate is
`private/analysis/piano-full-dtb-fd6266-impact-fixed/Piano-full-camera.dtb`,
1,209,103 bytes, SHA256
`b03853bb56cd38fcacc296f8824ecb4432154d7aebc04403fabe9ba04455b3c4`.
Same5804 nodes/2389 phandles and complete functional enablement remain.118 raw
diagnostics are retained. The manifest and diagnostic-impact.json map all119
original rows to function, effective status, actual source driver/table/hash,
consumption and post-correction result. No still-consumed blocker or unclassified
match remains **within this119-row review**; that is not proof of an entire boot.
Held/dummy rails, fw_devlink/sync-state behavior, firmware and the separate
SMMU handoff contract remain unverified. This analysis does not satisfy the
reference `/run/piano-smmu-ready` requirement.

Nine actual host tests pass, including real UART overlay application with
unchanged-provider/status checks and helper-versus-driver match distinction.
