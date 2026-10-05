#!/usr/bin/env bash
# Host-only behavior/ABI checks; never prepares a profile or contacts a device.
set -euo pipefail
sun_fs_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_fs_root"
sun_fs_includes=(
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
for sun_fs_test in ufs_filesystems ufs_blockio_lifetime launch_shell; do
  cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
    -ffunction-sections -fdata-sections "${sun_fs_includes[@]}" \
    "tools/test_${sun_fs_test}.c" -Wl,--gc-sections -o "build/test-${sun_fs_test}-asan"
  "build/test-${sun_fs_test}-asan"
done
sun_fs_arm_includes=(
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar \
  -ffreestanding -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter "${sun_fs_arm_includes[@]}" \
  bootprofiles/uefi-app/PianoUfsFileSystemProbe.c bootprofiles/uefi-app/PianoLaunchShell.c
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar \
  -ffreestanding -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -DPIANO_UFS_BLOCKIO -DPIANO_UFS_FILESYSTEMS -DPIANO_UFS_SHELL \
  "${sun_fs_arm_includes[@]}" bootprofiles/uefi-app/PianoUfsReadOnlyDma.c
python3 -m py_compile tools/prepare_gui_profile.py
