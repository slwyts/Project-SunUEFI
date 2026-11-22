#!/usr/bin/env python3
"""Run explicit offline checks; never prepares firmware or contacts a device.

The portable group runs on a source-only clone with Python's standard library.
The ci group adds public native checks for which CI prepares dependencies.
python-all discovers tests/unit and needs its documented local prerequisites.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from core_source import core_include_dirs
PORTABLE = (
    'tests/unit/test_product_contract.py',
    'tests/unit/test_usb_log_bounds.py',
    'tests/unit/test_collect_uefi_ramlog.py',
    'tests/unit/test_piano_bootfail.py',
    'tests/unit/test_build_integrity.py',
    'tests/unit/test_firmware_workspace.py',
    'tests/unit/test_product_early_dxe.py',
    'tests/unit/test_dma_log.py',
    'tests/unit/test_usb_diagnostic_host.py',
    'tests/unit/test_host_checks.py',
    'tests/unit/test_ci_build_targets.py',
    'tests/unit/test_check_patch_series.py',
    'tests/unit/test_prepare_sources.py',
    'tests/unit/test_vendor_inputs.py',
    'tests/unit/test_release_kernel.py',
    'tests/unit/test_release_rootfs.py',
    'tests/unit/test_install_piano.py',
    'tests/unit/test_assemble_rootfs.py',
    'tests/unit/test_shared_products.py',
    'tests/unit/test_ci_container.py',
)
CI_UNIT = (
    'test_boot_options.py',
    'test_build_input.py',
    'test_droidloom.py',
    'test_product_core.py',
    'test_late_handoff.py',
    'test_ufs_product_volume.py',
    'test_nv_journal.py',
    'test_nv_runtime_guard.py',
    'test_reset.py',
    'test_rtc_offset.py',
    'test_usb_ram_boot_controller.py',
    'test_gt9764.py',
    'test_build_libcamera_packages.py',
    'test_build_pipewire_packages.py',
    'test_build_plasma_camera_package.py',
    'test_build_steamos_rootfs.py',
    'test_build_arch_ffmpeg.py',
    'test_overlay_cache.py',
    'test_piano_camera.py',
    'test_camera.py',
    'test_bsp_package.py',
)
CI_NATIVE = (
    'test_dma_foundation.sh',
    'test_fastboot_boot.sh',
    'test_fastboot_launch.sh',
    'test_owned_smmu_diagnostic.sh',
    'test_owned_smmu_peer.sh',
    'test_uefi_setup.sh',
    'test_ufs_firmware.sh',
    'test_ufs_reset_shutdown.sh',
    'test_usb_debug.sh',
    'test_usb_fastboot.sh',
    'test_usb_ufs_fetch_app.sh',
)


def checks(root, group):
    unit = root / 'tests/unit'
    if group == 'python-all':
        return sorted(unit.glob('test_*.py'))
    portable = [root / name for name in PORTABLE]
    if group == 'ci':
        return [*portable, *(unit / name for name in CI_UNIT),
                *(root / 'tests/native' / name for name in CI_NATIVE)]
    return portable


def run_check(path, timeout):
    # Discover only this file. A subprocess isolates module names/import paths
    # between individual unit files and their temporary fixtures.
    native = path.suffix == '.sh'
    command = (['bash', str(path)] if native else
               [sys.executable, '-m', 'unittest', 'discover',
                '-s', str(path.parent), '-p', path.name, '-v'])
    # Core sources are grouped by subsystem but include each other by bare name.
    # The generated firmware module is flat, so tests get the same flat view.
    search = os.pathsep.join(str(path) for path in (ROOT / 'uefi/core', *core_include_dirs(ROOT)))
    python_path = os.pathsep.join(filter(None, (str(ROOT / 'tools'), os.environ.get('PYTHONPATH'))))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONPATH=python_path,
               CPATH=os.pathsep.join(filter(None, (search, os.environ.get('CPATH')))))
    try:
        result = subprocess.run(command, cwd=ROOT, env=env,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'FAILED', 0, 0, f'Exceeded {timeout}s timeout'
    output = result.stdout
    if native:
        if result.returncode:
            return 'FAILED', 1, 0, output
        if re.search(r'^Skipped:', output, re.MULTILINE):
            return 'SKIPPED', 1, 1, output
        return 'PASSED', 1, 0, output
    count = re.search(r'^Ran (\d+) tests? in ', output, re.MULTILINE)
    skipped = re.search(r'\bskipped=(\d+)', output)
    total = int(count.group(1)) if count else 0
    skips = int(skipped.group(1)) if skipped else 0
    if result.returncode or not total:
        return 'FAILED', total, skips, output
    return ('SKIPPED' if skips == total else 'PASSED_WITH_SKIPS' if skips else 'PASSED'), total, skips, output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', choices=('portable', 'ci', 'python-all'), default='portable')
    parser.add_argument('--list', action='store_true', help='List files without running tests')
    parser.add_argument('--pattern', action='append', help='Select test filenames within the group; may be repeated')
    parser.add_argument('--no-skips', action='store_true',
                        help='Fail if any selected check is skipped or a pattern matches no files')
    parser.add_argument('--timeout', type=int, default=120, help='Per-file timeout in seconds')
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    paths = checks(ROOT, args.group)
    if args.pattern:
        if args.no_skips:
            for pattern in args.pattern:
                if not any(path.match(pattern) for path in paths):
                    parser.error(f'No check matches {pattern!r}')
        paths = [path for path in paths if any(path.match(pattern) for pattern in args.pattern)]
    if not paths:
        parser.error('No checks selected')
    if args.list:
        print('\n'.join(str(path.relative_to(ROOT)) for path in paths))
        return 0
    print(f'Group: {args.group}; {len(paths)} test files. Not hardware validation.', flush=True)
    fail_on_skip = args.no_skips or args.group != 'python-all'
    failed = tests = skips = 0
    for path in paths:
        label = str(path.relative_to(ROOT))
        if not path.is_file():
            failed += 1
            print(f'FAILED {label}: missing file', flush=True)
            continue
        status, total, skipped, output = run_check(path, args.timeout)
        tests += total
        skips += skipped
        if fail_on_skip and skipped:
            status = 'FAILED'
        failed += int(status == 'FAILED')
        print(f'{status} {label}: {total} tests, {skipped} skipped', flush=True)
        if status == 'FAILED' or skipped:
            print(output, end='' if output.endswith('\n') else '\n', flush=True)
    print(f'Result: {tests} tests, {skips} skipped, {failed} failing files.', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
