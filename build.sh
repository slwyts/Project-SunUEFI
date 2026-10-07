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
record=json.loads(Path('build/release-7.2.9/source-manifest.json').read_text())
subprocess.run(['python3','tools/build_piano_full_kernel.py','--worktree',record['worktree'],
 '--commit',record['actual_commit'],'--root','LABEL=PIANOROOT',
 '--build-dir','build/kernels/release-7.2.9','--artifacts','artifacts/kernels/release-7.2.9'],check=True)
PY_KERNEL
    ;;
  mesa)
    bash upstream/piano-mesa-current/scripts/build-mesa-debs.sh \
      "${1:-$sunuefi_root/build/mesa}" 26.1.6-1~bpo13+1 ;;
  sensors)
    sensors_source="$sunuefi_root/upstream/piano-sensors-current"
    sensors_commit=65a92202db65ad13493b43fbf15d6abb3b7bcf92
    sensors_output="${1:-$sunuefi_root/build/sensors}"
    if [[ "$(git -C "$sensors_source" rev-parse HEAD)" != "$sensors_commit" ||
          -n "$(git -C "$sensors_source" status --porcelain --untracked-files=all)" ]]; then
      printf '%s\n' 'Sensors source HEAD/cleanliness mismatch; run ./build.sh sources.' >&2
      exit 1
    fi
    bash "$sensors_source/scripts/build-sensors-debs.sh" "$sensors_output"
    python3 - "$sensors_output" "$sensors_commit" <<'PY_SENSORS'
import json, sys
from pathlib import Path
(Path(sys.argv[1]) / 'SOURCE').write_text(json.dumps({
    'source_url': 'https://github.com/blu-sharky/piano-sensors.git',
    'source_commit': sys.argv[2],
}, indent=2) + '\n')
PY_SENSORS
    ;;
  bsp)
    python3 tools/package_bsp.py "$@" ;;
  rootfs)
    python3 tools/assemble_rootfs.py "$@" ;;
  release-rootfs)
    python3 tools/build_release_rootfs.py --kernel artifacts/kernels/release-7.2.9 \
      --source build/kernel-worktrees/release-7.2.9 --kernel-build build/kernels/release-7.2.9 \
      --output build/distros/release-7.2.9 --mesa-dir build/mesa/runtime \
      --sensors-dir build/sensors/runtime --execute "$@" ;;
  package|esp)
    python3 tools/package_release.py --uefi artifacts/product/PianoUEFI-product.img \
      --kernel artifacts/kernels/release-7.2.9 --dtb vendor/piano-linux/board.dtb \
      --cpu-model-overlay linux/dts/piano-cpu-model.dtso --panel-vendor auto \
      --dtb-overlay linux/dts/piano-audio-dmic-clock.dtso \
      --initramfs build/distros/release-7.2.9/initramfs/initramfs.cpio.gz \
      --rootfs build/distros/release-7.2.9/rootfs --rootfs-manifest build/distros/release-7.2.9/manifest.json \
      --output artifacts/release-7.2.9 --root-selector LABEL=PIANOROOT "$@" ;;
  all)
    "$0" sources
    "$0" uefi
    "$0" linux
    "$0" mesa
    "$0" sensors
    "$0" release-rootfs
    "$0" package "$@"
    ;;
  install) python3 tools/install_piano.py "$@" ;;
  installer) python3 tools/export_installer.py "$@" ;;
  help|-h|--help)
    printf '%s\n' 'Usage: ./build.sh sources|check|uefi|trampoline|module|linux|mesa|sensors|bsp|rootfs|release-rootfs|package|all|installer|install' \
      'Full builds need the documented builder environment; rootfs/Mesa/sensors run in a root ARM64 build container.' \
      'Building never partitions or flashes a tablet. install is a separate explicit command.' ;;
  *) printf 'Unknown command: %s\n' "$command" >&2; exit 2 ;;
esac
