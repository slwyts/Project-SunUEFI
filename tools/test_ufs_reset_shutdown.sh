#!/usr/bin/env bash
# Host-only; no prepare, firmware build, device access or reset.
set -euo pipefail
sun_reset_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_reset_root"
sun_reset_tmp="$(mktemp -d build/ufs-reset-host.XXXXXX)"
trap 'rm -rf -- "$sun_reset_tmp"' EXIT
sun_reset_inc=(-I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include)
for sun_reset_case in ufs_reset_shutdown ufs_probe; do
  cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie -ffunction-sections -fdata-sections "${sun_reset_inc[@]}" "tools/test_$sun_reset_case.c" -Wl,--gc-sections -o "$sun_reset_tmp/$sun_reset_case"
  "$sun_reset_tmp/$sun_reset_case"
done
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar -ffreestanding -fsyntax-only \
  -Wall -Wextra -Werror -Wno-unused-parameter -DPIANO_UFS_BLOCKIO=1 \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include \
  bootprofiles/uefi-app/PianoUfsReadOnlyDma.c bootprofiles/uefi-app/PianoUfsProbe.c
