#!/usr/bin/env bash
# Actual source EFI API mocks only; never LoadImage/StartImage on a device.
set -euo pipefail
sun_repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_repo"
sun_launch_dir="$(mktemp -d /tmp/sunuefi-launch-tests.XXXXXX)"
trap 'rm -rf -- "$sun_launch_dir"' EXIT
"${CC:-cc}" -std=gnu11 -fshort-wchar -Wall -Wextra -Werror -Wno-unused-parameter \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  tests/native/test_fastboot_launch.c -o "$sun_launch_dir/test"
ASAN_OPTIONS=detect_leaks=1 "$sun_launch_dir/test"
"${CLANG:-clang}" --target=aarch64-none-elf -ffreestanding -fshort-wchar -fsyntax-only \
  -Wall -Wextra -Werror \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  uefi/core/PianoFastbootLaunch.c uefi/core/PianoFastbootBoot.c \
  uefi/core/PianoFastbootDownloadBlob.c
python tests/unit/test_fastboot_download_blob.py
