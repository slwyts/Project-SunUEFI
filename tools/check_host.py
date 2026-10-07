#!/usr/bin/env python3
"""Run explicit offline checks; never prepares firmware or contacts a device.

The portable group runs on a source-only clone with Python's standard library.
python-all discovers tests/unit and needs its documented
local prerequisites. Native shell/C checks remain separate, explicit commands.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
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
    'tests/unit/test_vendor_inputs.py',
    'tests/unit/test_release_kernel.py',
    'tests/unit/test_release_rootfs.py',
    'tests/unit/test_install_piano.py',
    'tests/unit/test_assemble_rootfs.py',
    'tests/unit/test_bsp_package.py',
)


def checks(root, group):
    if group == 'portable':
        return [root / name for name in PORTABLE]
    return sorted((root / 'tests/unit').glob('test_*.py'))


def run_check(path, timeout):
    # Discover only this file. A subprocess isolates module names/import paths
    # between individual unit files and their temporary fixtures.
    command = [sys.executable, '-m', 'unittest', 'discover',
               '-s', str(path.parent), '-p', path.name, '-v']
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    try:
        result = subprocess.run(command, cwd=ROOT, env=env,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'FAILED', 0, 0, f'Exceeded {timeout}s timeout'
    output = result.stdout
    count = re.search(r'^Ran (\d+) tests? in ', output, re.MULTILINE)
    skipped = re.search(r'\bskipped=(\d+)', output)
    total = int(count.group(1)) if count else 0
    skips = int(skipped.group(1)) if skipped else 0
    if result.returncode or not total:
        return 'FAILED', total, skips, output
    return ('SKIPPED' if skips == total else 'PASSED_WITH_SKIPS' if skips else 'PASSED'), total, skips, output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', choices=('portable', 'python-all'), default='portable')
    parser.add_argument('--list', action='store_true', help='List files without running tests')
    parser.add_argument('--timeout', type=int, default=120, help='Per-file timeout in seconds')
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    paths = checks(ROOT, args.group)
    if not paths:
        parser.error('No checks selected')
    if args.list:
        print('\n'.join(str(path.relative_to(ROOT)) for path in paths))
        return 0
    print(f'Group: {args.group}; {len(paths)} Python files. Not hardware validation.', flush=True)
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
        # Portable is a promised self-contained set; missing coverage is a CI
        # failure. python-all reports legitimate optional local-input skips.
        bad = status == 'FAILED' or (args.group == 'portable' and skipped > 0)
        failed += int(bad)
        print(f'{status} {label}: {total} tests, {skipped} skipped', flush=True)
        if bad or skipped:
            print(output, end='' if output.endswith('\n') else '\n', flush=True)
    print(f'Result: {tests} tests, {skips} skipped, {failed} failing files. '
          'Native shell/C suites are not included.', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
