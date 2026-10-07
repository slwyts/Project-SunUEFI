#!/usr/bin/env bash
# Build the actual firmware storage source for ARM64, host only.
set -euo pipefail
sun_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_root"
sun_tools="$sun_root/build/host-tools/usr"
export LD_LIBRARY_PATH="$sun_tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p artifacts/ufs-block
"$sun_tools/bin/clang" -target aarch64-unknown-windows-msvc -ffreestanding -fshort-wchar \
  -fno-builtin -Oz -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  -c uefi/core/PianoUfsBlockIo.c -o artifacts/ufs-block/PianoUfsBlockIo.obj
cc -std=gnu11 -fshort-wchar -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  tests/native/test_ufs_block.c -o build/test-ufs-block
build/test-ufs-block
cc -std=gnu11 -fshort-wchar -fsanitize=address,undefined -g \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  tests/native/test_ufs_block.c -o build/test-ufs-block-asan
build/test-ufs-block-asan
printf 'ARM64 UFS block layer compiled; physical controller/PHY/DMA remains unverified.\n'
