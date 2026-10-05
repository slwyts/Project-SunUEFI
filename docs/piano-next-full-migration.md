# Complete Piano profile migration to current mainline

Verified online on2026-10-06: official torvalds/master is
`67f0943b394d920b6c142aad8c6af94340342ae7`, committed2026-10-05 14:36:06UTC.
It merges SELinux fixes on top of `a90ee4305c4a5df72c11b31dacfdc76e00fcf78a`.
The rc6 release date is2026-10-04. Compared with rc6, only
`security/selinux/avc.c` and `hooks.c` change; it adds no Piano hardware support.
Sources: [official master commit](https://github.com/torvalds/linux/commit/67f0943b394d920b6c142aad8c6af94340342ae7)
and [kernel.org release feed](https://www.kernel.org/releases.json).

The exact commit was fetched only to
`refs/research/2026-10-06/torvalds-master`. Existing stable7a33, next7704,
remote-tracking pins, gitlink, full/SMMU candidates and rescue artifacts were
not moved or edited. The clean ef43c0 panel worktree remains unchanged.

## Actual tree and upstream comparison

The complete352508 GNOME hardware kernel changes134 paths relative to vanilla
500df175a7f9. Direct Git blob comparison with current master gives:

- **18 identical upstream paths**: no need to reintroduce those whole-file
  changes, including common clock helpers, q6v5 header, Q6APM/PRM/AFE/LPASS port
  helpers, generic Qualcomm sound-card helpers and their port bindings.
- **30 paths absent upstream**: device implementations/bindings/configs remain
  necessary. This includes NT36532, WN8030, NT36532E touch, FS19xx, MCA, SC8541,
  OV32D40 and Gen4/980 CAMSS additions. The legacy Piano DTS/configs are source
  material, not proof that they match the current folded board graph.
- **86 different or mixed paths**: compare device hunks against current APIs;
  do not replace entire DWC3/PAS/CAMSS/ATH12K/MSM files with their old snapshots.
  Such replacement would remove newer upstream behavior even if a device hunk
  was needed.

`artifacts/kernel-research/2026-10-06/next-full-tree-audit.json` preserves all
134 path classifications and base/full/upstream blob IDs. This is byte/tree
evidence, not device validation or automatic patch acceptance.

Eight original generic commits are positively proven ancestors of current
master using actual Git graph queries: DSI packet multi-slice `ffa88de8`, MSM
DSI `ce73a5db`, q6v5 handover `bb7c5d6f`, PAS attach `34b8b2d7`, Q6APM TDM
operations `4d084017`, QAIF clock rename `c41ac868`, TDM slot distinction
`0a9e00d5`, and SC8280 TDM error handling `8593dc5f`. Keep upstream versions
and omit those redundant backport hunks. This does not authorize dropping
all device changes from the mixed display/audio squash. The detailed positive
query results are `generic-upstream-ancestry.json`; no shallow negative was
turned into an upstream claim.

## Shortest reviewable canonical stack

| Group | Reuse/port unit | Residual that must remain |
| --- | --- | --- |
| Panel/backlight | Existing four canonical NT36532/paired-KTZ commits | Real Piano variants, dual DSI/DSC, paired backlight semantics |
| Input | One WN8030 commit, then one NT36532E SPI/firmware/binding commit | Keyboard/touchpad/HID UAPI, RAM firmware loading, panel follower and stylus/touch behavior |
| Battery/power | One MCA+SC8541/bindings/config unit | Actual protocol transport and controlled charging behavior; do not substitute QCOM battmgr for MCA |
| Audio/ADSP | FS19xx binding/codec and Piano sound-card/Q6APM device residual unit | TDM channel mapping, firmware presets, PMIC ADSP aliases/rails; upstream handover/QAIF remain upstream |
| WLAN/BT | Peach/WCN7861 pwrseq/QCA/ATH12K device residual units | Firmware/board data, AOSS/PDC/PMU sequencing and RF clock; current PCIe/MSI/PHY APIs need review |
| GPU/display core | A830/GMU/DPU/DSC device residual unit | Do not duplicate upstream multi-slice support or discard current MSM APIs |
| USB/providers | Empty-extcon overlay handling and QCOM ICC policy as separately reviewed hunks | These local behavior changes are not proven upstream; preserve correct ACPI behavior and provider error semantics |
| IOMMU/ownership | Rebase the independently reviewed1fd91/74c517 stack after required3525 adoption residual | No USB-ancestor cloning, fake marker or global disable; not copied from the older unsafe last-CB probe |
| Camera |352508 CAMSS/Gen4/TFE/OV32D40 additions as one dependency-aware unit | Camera commit changes20 files/over7k added lines; not a tiny board-only addition. Preserve current upstream CAMSS resources/APIs |
| Complete profile/DT | Full public piano_rootfs profile plus explicit RAM-root override and independently folded board DT | Same complete source/services/firmware scope; no Android userdata/growfs policy and no RAM-smoke fallback |

A file count is not a commit count. Adjacent Kconfig/Makefile/binding edits can
be grouped with their real driver, but wide squash commits should not be replayed
blindly. Board wiring stays tied to the current-ROM/live/published folding record,
not rebuilt from the early MTP skeleton. SMMU and reliable DDR handoff remain
independent admission requirements for either stable or next.

## Actual canonical progress and object validation

An independent worktree `build/kernel-worktrees/piano-next-full-audit`, branch
`codex/piano-next-full-audit`, starts at67f0943 and carries:

1. `a49673b80d1`: NT36532 variant binding.
2. `ff4ce6f43f1`: dual-DSI NT36532 driver/Kconfig/Makefile.
3. `e3ae82aca8c`: paired KTZ binding.
4. `1f80b2cbf1e`: paired-secondary KTZ driver.
5. `86645fad3c4eedf09d1b976333c227e5aecf8a98`: complete WN8030 keyboard/touchpad
   driver, binding and UAPI from pinned f8afe7. Only the upstream HID Makefile
   insertion context needed adaptation; no generic HID subsystem was replaced.

Real LLVM23.1.1/AArch64 `olddefconfig` and object compilation completed **exit0**
for WN8030, NT36532 and KTZ8866 in the independent output
`build/kernel-topics/piano-next-full-audit`. Log:
`build/logs/piano-next-full-audit-object.log`; exact object bytes/hashes/config
and commit are in `next-input-display-object-validation.json`.

This is three device object builds, **not a complete GNOME Image/modules build**.
The initial copied full config cannot enable missing next drivers merely by
retaining their old symbols; remaining groups must be ported before a complete
profile requirement audit. No new kernel or DT was run on the tablet.

## Complete device groups now ported

The same independent next topic continued beyond the three initial objects.
Each group uses base500df→full352508 device hunks with a real three-way merge;
upstream APIs and entries remain instead of whole-file downgrade:

| Canonical commit | Device group | Actual AArch64 object result |
| --- | --- | --- |
|01305507292|Full NT36532E SPI touch/stylus and firmware update|2 objects, exit0|
|14a959e2ca0|MCA/SC8541, PMIC GLINK/PD mapper/battmgr|5 objects, exit0|
|f153944cf21|FS19xx/Piano card/AudioReach TDM residual|6 objects, exit0|
|4c33c9a0a03|Peach/WCN7861/QCA/ATH12K/PCIe/PHY|9 objects, exit0; PCIe2 recompiled after fixes|
|6ed91246d88|A830/DPU/DSC/UBWC residuals|8 objects, exit0|
|10e3e569f00|CAMSS Gen4/TFE/CSID980/OV32D40/CCI|10 objects, exit0|
|b32a01f03a5|PAS/q6v5 device residual and DWC3 behavior|4 objects, exit0|
|1bab7e13a85|Complete public profile and SoC/board source|50 profile requirements and Piano DTB, exit0|

Conflict resolutions preserve upstream BQ25630 entries, the new MSI decision
before host initialization, scoped CCI OF-child iteration, current UBWC data
refactor and newer SOCCP/Eliza remoteproc resources. Piano aliases and device
extensions coexist with those changes. Already-upstream DSI multi-slice and
q6v5 handover hunks were not replayed. The audio base patch shrank from2301 added
lines to1078 retained device lines after upstream deduplication.

Source HEAD is `1bab7e13a85f7ae02b80333bd7602ccd4bde4551`, clean. Full
`piano_defconfig + piano_rootfs.config + existing RAM CMDLINE override` passes
all50 actual complete-profile requirements, including MCA/SC8541/FS19xx/input/
wireless/MSM/camera. No function was disabled to pass an object build. Public
BT_LE=n remains its disclosed default, not an introduced workaround.

Full `Image modules` compilation is running in independent
`build/kernel-topics/piano-next-full-audit` with four jobs; log
`build/logs/piano-next-full-image-modules.log`. Until the process exits and module
link/install checks finish, it is **incomplete**, not a full build result. Root
owns the new translated-domain/SMMU ownership stack, which will be ported as a
separate group after it freezes. This build does not certify SMMU handoff,
full DDR, a distribution boot or hardware functionality.
