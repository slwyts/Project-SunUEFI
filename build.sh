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
  module) python3 tools/build_android_product_module.py "$@" ;;
  module-package) python3 tools/build_android_module.py "$@" ;;
  boot-request) python3 tools/build_boot_request.py "$@" ;;
  boot-repack) python3 tools/build_boot_repack.py "$@" ;;
  ffmpeg) python3 tools/build_ffmpeg_packages.py "$@" ;;
  steamos-rootfs) python3 tools/build_steamos_rootfs.py "$@" ;;
  nyarch-rootfs|nyarch-gnome-rootfs) python3 tools/build_nyarch_rootfs.py "$@" ;;
  gsd) python3 tools/build_gsd_packages.py "$@" ;;
  upower) python3 tools/desktop/build_upower_packages.py "$@" ;;
  gdm) python3 tools/desktop/build_gdm_packages.py "$@" ;;
  pipewire) python3 tools/desktop/build_pipewire_packages.py "$@" ;;
  mutter)
    if (($#)); then
      python3 tools/build_mutter_packages.py "$@"
    else
      python3 tools/build_mutter_packages.py --sysroot build/distros/release/rootfs
    fi ;;
  linux)
    python3 tools/prepare_release_kernel.py --refresh
    python3 tools/build/kernel.py "$@"
    ;;
  mesa) python3 tools/build/mesa.py "$@" ;;
  sensors) python3 tools/build/sensors.py "$@" ;;
  display-calibration) python3 tools/display/build_output_color.py "$@" ;;
  bsp)
    python3 tools/package_bsp.py "$@" ;;
  rootfs)
    python3 tools/assemble_rootfs.py "$@" ;;
  release-rootfs)
    sunuefi_release_source="$(python3 tools/build/kernel_worktree.py)"
    python3 tools/build_release_rootfs.py --kernel artifacts/kernels/release \
      --source "$sunuefi_release_source" --kernel-build build/kernels/release \
      --output build/distros/release --mesa-dir build/mesa/runtime \
      --sensors-dir build/sensors/runtime --ffmpeg-dir build/ffmpeg/runtime --execute "$@" ;;
  package|esp)
    if [[ "$command" == esp ]]; then set -- --esp-only "$@"; fi
    python3 tools/package/release.py "$@"
    ;;
  all)
    "$0" sources
    "$0" uefi
    "$0" module
    "$0" linux
    "$0" mesa
    "$0" sensors
    "$0" ffmpeg --install-dependencies
    "$0" release-rootfs
    "$0" package "$@"
    ;;
  install) python3 tools/install_piano.py "$@" ;;
  installer) python3 tools/export_installer.py "$@" ;;
  help|-h|--help)
    printf '%s\n' 'Usage: ./build.sh sources|check|uefi|trampoline|module|module-package|boot-request|boot-repack|linux|mesa|sensors|display-calibration|ffmpeg|gsd|upower|gdm|pipewire|mutter|bsp|rootfs|release-rootfs|steamos-rootfs|package|all|installer|install' \
      'Full builds need the documented builder environment; rootfs/Mesa/sensors run in a root ARM64 build container.' \
      'release-rootfs reads its kernel source worktree from build/release/source-manifest.json.' \
      'Building never partitions or flashes a tablet. install is a separate explicit command.' ;;
  *) printf 'Unknown command: %s\n' "$command" >&2; exit 2 ;;
esac
