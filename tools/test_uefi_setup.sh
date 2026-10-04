#!/usr/bin/env bash
# Host-only checks; never prepares or builds a firmware profile.
set -euo pipefail
sun_setup_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_setup_root"
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -ffunction-sections -fdata-sections \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include \
  tools/test_launch_setup.c -Wl,--gc-sections -o build/test-launch-setup-asan
build/test-launch-setup-asan
build/host-tools/usr/bin/clang --target=aarch64-windows-msvc -fshort-wchar \
  -ffreestanding -fsyntax-only -Werror \
  -D_PCD_GET_MODE_BOOL_PcdEmuVariableNvModeEnable=1 \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/AArch64 \
  -I upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include \
  bootprofiles/uefi-app/PianoLaunchSetup.c
python3 tools/test_setup_profile.py
python3 -m py_compile tools/prepare_gui_profile.py
