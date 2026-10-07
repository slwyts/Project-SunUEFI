#!/usr/bin/env bash
# Host-only actual-source tests. No prepare, firmware build or device command.
set -euo pipefail
sun_repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_repo"
sun_test_dir="$(mktemp -d /tmp/sunuefi-usb-fastboot-tests.XXXXXX)"
trap 'rm -rf -- "$sun_test_dir"' EXIT
sun_flags=(-std=gnu11 -fshort-wchar -Wall -Wextra -Werror -Wno-unused-parameter
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include)
for sun_test in test_usb_control test_usb_session test_usb_diagnostic test_fastboot test_usb_reboot_lifecycle test_fastboot_screen; do
  "${CC:-cc}" "${sun_flags[@]}" "tools/$sun_test.c" -lcrypto -o "$sun_test_dir/$sun_test"
  "$sun_test_dir/$sun_test"
done
for sun_test in test_fastboot_fetch test_fastboot_block_read test_usb_fastboot test_usb_screenshot_route test_usb_coexist test_usb_boot_request; do
  "${CC:-cc}" "${sun_flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "tools/$sun_test.c" -lcrypto -o "$sun_test_dir/$sun_test"
  ASAN_OPTIONS=detect_leaks=1 "$sun_test_dir/$sun_test"
done
python tests/unit/test_usb_diagnostic_host.py
