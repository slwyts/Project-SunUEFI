#!/usr/bin/env bash
# Host-only. No prepare CLI, staged firmware, controller/device or real writes.
set -euo pipefail
sun_transport_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_transport_root"
sun_transport_tmp="$(mktemp -d build/ufs-write-host.XXXXXX)"
trap 'rm -rf -- "$sun_transport_tmp"' EXIT
# Render the pinned, verified PC bytes into this disposable host-test include.
# Do not invoke prepare_ufs_write_test/main or mutate any firmware staging tree.
python3 - "$sun_transport_tmp/PianoUfsWriteTestBaseline.h" <<'PY'
import importlib.util,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('piano_attestation',Path('tools/prepare_ufs_write_test.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
Path(sys.argv[1]).write_bytes(module._render_header(module.verify_capture()))
PY
sun_transport_includes=(
  -I "$sun_transport_tmp"
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
for sun_transport_mode in restore preflight; do
  sun_transport_define=()
  if [[ "$sun_transport_mode" == preflight ]]; then sun_transport_define=(-DPIANO_TEST_PREFLIGHT=1); fi
  cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
    -ffunction-sections -fdata-sections "${sun_transport_includes[@]}" "${sun_transport_define[@]}" \
    tools/test_ufs_write_transport.c bootprofiles/uefi-app/PianoUfsWriteTest.c \
    bootprofiles/uefi-app/PianoGpt.c -Wl,--gc-sections -lcrypto -o "$sun_transport_tmp/test-$sun_transport_mode"
  "$sun_transport_tmp/test-$sun_transport_mode"
done
sun_transport_arm_includes=(
  -I "$sun_transport_tmp"
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
for sun_transport_mode in PIANO_UFS_WRITE_PREFLIGHT PIANO_UFS_WRITE_RESTORE_TEST; do
  build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar -ffreestanding -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter \
    -DPIANO_UFS_BLOCKIO=1 -DPIANO_UFS_WRITE_TEST=1 -D"$sun_transport_mode"=1 \
    "${sun_transport_arm_includes[@]}" bootprofiles/uefi-app/PianoUfsReadOnlyDma.c
done
