#!/usr/bin/env bash
# Host-only. Reads durable PC backup fixtures; transport callbacks are mocks.
set -euo pipefail
sun_write_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_write_root"
sun_write_fixture="${1:-private/captures/ufs-test-area-1}"
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include \
  tests/native/test_ufs_write_transaction.c uefi/core/PianoUfsWriteTest.c \
  uefi/core/PianoGpt.c -lcrypto -o build/test-ufs-write-transaction-asan
build/test-ufs-write-transaction-asan "$sun_write_fixture"
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar \
  -ffreestanding -fsyntax-only -Werror \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include \
  uefi/core/PianoUfsWriteTest.c
