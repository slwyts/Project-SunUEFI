#!/usr/bin/env bash
# Host-only; actual app source with fake owners, no prepare/build/device/reset.
set -euo pipefail
sun_fetch_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_fetch_root"
sun_fetch_tmp="$(mktemp -d build/fetch-app-host.XXXXXX)"
trap 'rm -rf -- "$sun_fetch_tmp"' EXIT
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  tests/native/test_usb_ufs_fetch_app.c -o "$sun_fetch_tmp/fetch-app"
"$sun_fetch_tmp/fetch-app"
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar -ffreestanding -fsyntax-only \
  -Wall -Wextra -Werror -Wno-unused-parameter \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  uefi/core/PianoUsbUfsFetch.c
