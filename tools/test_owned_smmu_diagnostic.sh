#!/usr/bin/env bash
# Host-only source tests and production AArch64 syntax; no prepare/device/build.
set -euo pipefail
sun_diag_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_diag_root"
sun_diag_tmp="$(mktemp -d build/owned-smmu-diag.XXXXXX)"
trap 'rm -rf -- "$sun_diag_tmp"' EXIT
sun_diag_inc=(-I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include)
for sun_diag_test in owned_smmu owned_smmu_diagnostic; do
  cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -ffunction-sections -fdata-sections "${sun_diag_inc[@]}" "tools/test_$sun_diag_test.c" \
    -Wl,--gc-sections -o "$sun_diag_tmp/$sun_diag_test"
  "$sun_diag_tmp/$sun_diag_test"
done
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar -ffreestanding -fsyntax-only \
  -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include \
  bootprofiles/uefi-app/PianoOwnedSmmu.c
