#!/usr/bin/env bash
# Pure parser, loopback-only CLI and offline memory audit. No device/build.
set -euo pipefail
sun_repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_repo"
sun_boot_dir="$(mktemp -d /tmp/sunuefi-boot-parser-tests.XXXXXX)"
trap 'rm -rf -- "$sun_boot_dir"' EXIT
sun_flags=(-std=gnu11 -fshort-wchar -Wall -Wextra -Werror
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64)
"${CC:-cc}" "${sun_flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/native/test_fastboot_boot.c -o "$sun_boot_dir/parser"
ASAN_OPTIONS=detect_leaks=1 "$sun_boot_dir/parser"
"${CC:-cc}" "${sun_flags[@]}" tools/inspect_fastboot_boot.c -o "$sun_boot_dir/inspect"
for sun_fixture in artifacts/kernel-topics/piano-efi-entry-debug/Image \
  private/captures/2026-10-03-piano/boot_a.img private/captures/2026-10-03-piano/init_boot_a.img; do
  if [[ -f "$sun_fixture" ]]; then "$sun_boot_dir/inspect" "$sun_fixture"; fi
done
python tests/unit/test_fastboot_boot_cli.py
python tests/unit/test_fastboot_ram_pool.py
