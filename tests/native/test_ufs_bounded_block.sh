#!/usr/bin/env bash
# Host-only. No prepare CLI, staged firmware, controller/device or real writes.
set -euo pipefail
sun_transport_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
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
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -ffunction-sections -fdata-sections "${sun_transport_includes[@]}" \
  tests/native/test_ufs_bounded_block.c uefi/core/PianoUfsBoundedBlock.c \
  uefi/core/PianoUfsBoundedLayout.c uefi/core/PianoUfsWriteTest.c \
  uefi/core/PianoGpt.c -Wl,--gc-sections -lcrypto -o "$sun_transport_tmp/bounded-block"
"$sun_transport_tmp/bounded-block"
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -ffunction-sections -fdata-sections "${sun_transport_includes[@]}" \
  tests/native/test_ufs_bounded_transport.c uefi/core/PianoUfsBoundedLayout.c \
  uefi/core/PianoUfsBoundedBlock.c -Wl,--gc-sections -o "$sun_transport_tmp/bounded-transport"
"$sun_transport_tmp/bounded-transport"
sun_transport_arm_includes=(
  -I "$sun_transport_tmp"
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar -ffreestanding -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter \
  -DPIANO_UFS_BLOCKIO=1 -DPIANO_UFS_BOUNDED_VOLUME=1 \
  "${sun_transport_arm_includes[@]}" uefi/core/PianoUfsReadOnlyDma.c \
  uefi/core/PianoUfsBoundedBlock.c uefi/core/PianoUfsBoundedLayout.c
