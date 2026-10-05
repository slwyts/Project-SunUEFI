#!/usr/bin/env bash
# Actual source host-only proof/contract validation, never enable strictClose.
set -euo pipefail
sun_peer_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$sun_peer_root"
sun_peer_tmp="$(mktemp -d /tmp/sunuefi-owned-peer.XXXXXX)"
trap 'rm -rf -- "$sun_peer_tmp"' EXIT
cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
  -ffunction-sections -fdata-sections \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include \
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64 \
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include \
  tools/test_owned_smmu_peer.c -Wl,--gc-sections -o "$sun_peer_tmp/peer"
ASAN_OPTIONS=detect_leaks=1 "$sun_peer_tmp/peer"
