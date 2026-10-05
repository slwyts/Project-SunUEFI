# Pinned userspace-debug kernel builds

The userspace-debug mode merges the existing RAM fragment with
`configs/linux/piano-userspace-debug.config`. It provides distribution and USB
debug facilities while keeping UFS and its PHY as modules. This is not a
validated distribution or board-driver result; no userdata root or automatic
module-loading policy is selected here.

The Stable build completed from the clean pinned commit
`7a33c60fd6eda8a9c20dfda636d4e0a4efa4cbb8`, release
`7.2.6-00060-g7a33c60fd6ed`. Its actual EFI-stub Image is 41,298,432 bytes,
SHA256 `0a790203c19ea722fc5b107a6fbadf20df7e57f09d96b5d8c790ad702e18d392`.
The 1,629 installed module files total 1,426,519,440 bytes including their build
debug data. Image, config and every module hash were checked against the
manifest. No module or kernel was loaded onto the tablet.

Artifacts are in `artifacts/kernels/stable/userspace-debug/`, with complete
source/config/compiler/module provenance in `manifest.json`. The build log is
`build/logs/piano-stable-userspace-build.log`. Final configuration includes
DWC3/QCOM/dual-role/configfs ACM/NCM and EXT4, with UFS/QCOM UFS PHY modules.
Compiled configuration does not prove role switching, PHY, storage or USB
enumeration on hardware. A validated complete board DTB and OS handoff remain
required before those drivers can be exercised.

The corresponding Next build completed from its unchanged pinned commit
`7704c4c5bb127673b4f0ead839919db573559e38`, release
`7.3.0-rc5-g7704c4c5bb12`. Its Image is 42,027,520 bytes, SHA256
`4353ae4d3dd5255b19b567369791412bd46365962ca2c02191e08d37994573b9`;
1,684 module hashes and the actual Image/config hashes were checked. Its log is
`build/logs/piano-next-userspace-build.log`. These generic userspace-debug
configurations do not enable every Piano-specific device; the new complete
candidate uses the public `piano_rootfs.config` instead. Source branches, firmware
submodule gitlink and the already verified Stable/Next RAM rescue bundles were
not moved or replaced by either build.
