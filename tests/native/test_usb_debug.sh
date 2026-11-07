#!/usr/bin/env bash
# Host-only verification of firmware code; no adb/fastboot/device access.
set -euo pipefail
case "$(uname -m)" in aarch64|arm64) sun_bind=AArch64 ;; *) sun_bind=X64 ;; esac
sun_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_root"
sun_flags=(-std=gnu11 -fshort-wchar -ffunction-sections -fdata-sections
  -Wall -Wextra -Werror -Wno-unused-parameter
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/$sun_bind
  -I upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include)
mkdir -p build
cc "${sun_flags[@]}" tests/native/test_usb_debug.c -Wl,--gc-sections -lcrypto -o build/test-usb-debug
build/test-usb-debug
cc -std=gnu11 -Wall -Wextra -Werror tests/native/test_touch_input.c -o build/test-touch-input
build/test-touch-input
cc -std=gnu11 -Wall -Wextra -Werror tests/native/test_y700_touch_decode.c -o build/test-y700-touch-decode
build/test-y700-touch-decode
cc "${sun_flags[@]}" tests/native/test_usb_power.c -Wl,--gc-sections -o build/test-usb-power
build/test-usb-power
