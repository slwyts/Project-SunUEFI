# Integrated Piano Linux candidate

The user's target is one integrated, full-function Linux attempt. This candidate
reuses the current public GNOME/hardware startup chain rather than treating a
RAM-smoke or generic userspace-debug kernel as complete Piano enablement.
All original Android data and boot partitions remain preserved.

## Frozen source set

| Component | Exact source |
| --- | --- |
| Public integration | xiaomi-piano-linux `db63c161d6bff44e87a34be66136c4db2a94bcf9` |
| Full kernel | linux-piano `352508459733d3e6d349ea5581a8dd2fd8bb4180` |
| GNOME/services/DT overlay | debian-piano `fd6266d73f3442b23362260c3aa0c86782e0b52c` |
| Firmware | piano-firmware `05fc37c09ca8146eecfc4920f8485479db140ae5` |
| Adreno Mesa | piano-mesa `a89c1e46eef3e161520b084479928aaa5a8e8732` |
| Official board DTS | MiCode/kernel_devicetree `7333e86a6e64060ba1c9e93678fc75b5f591c3aa` |
| Official downstream kernel | MiCode/Xiaomi_Kernel_OpenSource `45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71` |

The kernel is checked out in the independent repository's
`codex/piano-full-integration` worktree, at
`build/kernel-worktrees/piano-full-integration`. Existing Stable/Next pins and
the firmware gitlink are unchanged. Relative to our old7a33 kernel, this public
HEAD adds one camera/CAMSS/TFE commit; most device code was already present.
The missing link was the full profile and matching DT/userspace/firmware chain.

`tools/build_piano_full_kernel.py` uses the actual public piano_defconfig and
piano_rootfs.config. The local fragment replaces only userdata-root arguments
with the RAM-root policy. It checks 47 real configuration requirements, including
MSM/GPU, ADSP/audio, MCA/SC8541, touch, WN8030, wireless and camera; installed
modules are stripped and verified against the exact kernel release. The public
BT_LE=n default is disclosed, not silently counted as full BLE support.
Its current process/build completion must be checked through the live process
and final manifest, not inferred from a pending file. The build log is
`build/logs/piano-full-integration-build.log`.

## ROM and final device tree

Current Android runtime evidence is in
`private/analysis/android-board-runtime-2026-10-05/`. ROM OS3.0.309.0.WPYCNXM
selects vendor DTB entry4 and factory DTBO entry0. Real libfdt composition finds
5,526 identical nodes,31 changed and4 new relative to the live5,561-node tree;
most property differences are symbol trimming, with51 real runtime corrections.
The graph preserves2,242 phandles and738 bus devices across all hardware chains.

The public camera top-level overlay includes the full display/input/audio/radio
chain and embedded stock fragments. It must be compiled once and folded once
onto the selected ROM base. Never apply each included layer separately or
blindly replay its stock prefix onto an already-composed runtime tree. Current
ROM wiring and runtime corrections require path-based phandle remapping; explicit
public mainline overrides remain separately identified. Raw compilation warnings
and unresolved binding/dependency evidence are retained. DTB construction is
host evidence, not hardware acceptance.

## Complete userspace and data preservation

The public rootfs-init/fstab requires Android userdata and enables growfs. That
policy is incompatible with this task. Our RAM entry keeps the complete hardware
service overlay, removes the Android block root and growfs policy, masks the
persistent16GiB swapfile, and marks internal UFS block devices readonly/ignored
by desktop automount. `tools/stage_piano_full_userspace.py` stages this change
without editing the public source. No Android partitions are mounted or flashed.
The entry refuses missing real systemd, matching modules or audited hardware
preparation instead of manufacturing a SMMU-ready marker.

The requested RAM-root ceiling is now4GiB (4,294,967,296 bytes). `pianoinit`
checks the original unpacked tree and MemAvailable, mounts a4GiB-capped tmpfs,
copies the complete GNU root while excluding virtual mounts, moves those mounts
and switches root. The copy needs additional source-sized RAM plus512MiB
headroom; the limit is not an upfront4GiB reservation. This path has not been
boot-tested yet and requires complete memory publication. Root staging records
the ceiling explicitly. Original userdata and boot-slot mutation services remain
excluded from the RAM policy.

The ceiling is distinct from the compressed fastboot payload limit. Firmware's
actual current limit remains64MiB and its planned download capacity remains1GiB
pending the validated DDR allocator. Standard fastboot uses an8-hex-digit DATA
length; exact4GiB cannot fit in that field. Any larger download policy must
respect that protocol bound rather than wrap a32-bit length to zero. A4GiB
unpacked root can be delivered as a smaller compressed package once the loading
and memory contracts are ready.

The official Debian13.7 ARM64 OCI layer and uncompressed diff ID were verified;
real ARM64 Bash/dpkg/glibc/child-exec run under workspace QEMU. QEMU user-mode
does not validate PID1 or a Piano kernel boot. The derived systemd installation
encountered a PRoot path assertion and remains incomplete; this is being repaired
with a private user-namespace runtime. GNOME, patched Mesa and full dependency
configuration are required before the candidate may be called assembled.

Firmware's99 files total55,387,076 bytes and all SHA256SUMS passed locally.
The source release is stockOS3.0.308, while this device is309; device-specific
calibration and changed firmware must still be matched, not guessed. The
published hardware success statements are reference evidence, not our device's
acceptance. No full Linux candidate has yet been booted on this tablet.
