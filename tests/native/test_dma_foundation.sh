#!/usr/bin/env bash
# Host-only tests of the actual shared DMA/SMMU and read-only UFS sources.
set -euo pipefail
sun_dma_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$sun_dma_root"
sun_dma_includes=(
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include
  -I upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64
  -I upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include
  -I upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include
)
for sun_dma_test in dma smmu_snapshot io_pgtable owned_smmu ufs_dma_layout ufs_readonly_dma ufs_probe gpt readonly_block; do
  cc -std=gnu11 -fshort-wchar -g -fsanitize=address,undefined -fno-pie -no-pie \
    -ffunction-sections -fdata-sections "${sun_dma_includes[@]}" \
    "tests/native/test_${sun_dma_test}.c" -Wl,--gc-sections -o "build/test-${sun_dma_test}-asan"
  "build/test-${sun_dma_test}-asan"
done
.venv/bin/python tests/unit/test_dma_log.py
