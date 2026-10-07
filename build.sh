#!/usr/bin/env bash
set -euo pipefail
sunuefi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$sunuefi_root"
command="${1:-help}"
if (($#)); then shift; fi
case "$command" in
  sources) python3 tools/prepare_sources.py "$@" ;;
  check) python3 tools/check_host.py --group portable "$@" ;;
  uefi) make uefi ;;
  trampoline) python3 tools/package_product_trampoline.py "$@" ;;
  module) python3 tools/build_android_module.py "$@" ;;
  linux)
    python3 tools/prepare_release_kernel.py
    python3 - <<'PY_KERNEL'
import json, subprocess
from pathlib import Path
record=json.loads(Path('build/release-kernel/source-manifest.json').read_text())
subprocess.run(['python3','tools/build_piano_full_kernel.py','--worktree',record['worktree'],
 '--commit',record['actual_commit'],'--root','LABEL=PIANOROOT',
 '--build-dir','build/kernels/release','--artifacts','artifacts/kernels/release'],check=True)
PY_KERNEL
    ;;
  mesa)
    bash upstream/piano-mesa-current/scripts/build-mesa-debs.sh \
      "${1:-$sunuefi_root/build/mesa}" 26.1.6-1~bpo13+1 ;;
  bsp)
    python3 tools/package_bsp.py "$@" ;;
  rootfs)
    python3 tools/assemble_rootfs.py "$@" ;;
  release-rootfs)
    python3 tools/build_release_rootfs.py --kernel artifacts/kernels/release \
      --source build/kernel-worktrees/release-kernel --kernel-build build/kernels/release \
      --output build/distros/release --mesa-dir build/mesa/runtime --execute "$@" ;;
  package|esp)
    python3 tools/package_release.py --uefi artifacts/product/PianoUEFI-product.img \
      --kernel artifacts/kernels/release --dtb vendor/piano-linux/board.dtb \
      --initramfs build/distros/release/initramfs/initramfs.cpio.gz \
      --rootfs build/distros/release/rootfs --rootfs-manifest build/distros/release/manifest.json \
      --output artifacts/release --root-selector LABEL=PIANOROOT "$@" ;;
  all)
    "$0" sources
    "$0" uefi
    "$0" linux
    "$0" mesa
    "$0" release-rootfs
    "$0" package "$@"
    ;;
  install) python3 tools/install_piano.py "$@" ;;
  installer) python3 tools/export_installer.py "$@" ;;
  help|-h|--help)
    printf '%s\n' 'Usage: ./build.sh sources|check|uefi|trampoline|module|linux|mesa|bsp|rootfs|release-rootfs|package|all|installer|install' \
      'Full builds need the documented builder environment; rootfs/Mesa run in a root ARM64 build container.' \
      'Building never partitions or flashes a tablet. install is a separate explicit command.' ;;
  *) printf 'Unknown command: %s\n' "$command" >&2; exit 2 ;;
esac
