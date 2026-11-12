#!/usr/bin/env python3
"""Read and compare the 26 captured boot partition hashes; never writes device files."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

BASES = ('boot', 'init_boot', 'vendor_boot', 'dtbo', 'vbmeta', 'vbmeta_system',
         'recovery', 'uefi', 'abl', 'xbl', 'xbl_config', 'hyp', 'devcfg')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--test-id', type=int, required=True)
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    capture = json.loads((root / 'private/captures/2026-10-03-piano/manifest.json').read_text())
    names = [f'{base}_{slot}' for base in BASES for slot in ('a', 'b')]
    paths = [f'/dev/block/by-name/{name}' for name in names]
    command = 'sha256sum ' + ' '.join(shlex.quote(path) for path in paths)
    adb = ['adb', '-s', args.serial]
    result = subprocess.run(adb + ['exec-out', 'su -c ' + shlex.quote(command)],
                            capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise SystemExit('Device hash read failed: ' + result.stderr.strip())
    hashes = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[1] not in paths or len(fields[0]) != 64:
            raise SystemExit('Unexpected sha256sum output')
        hashes[Path(fields[1]).name] = fields[0]
    if set(hashes) != set(names):
        raise SystemExit('Incomplete boot partition hashes')
    comparisons = {name: {'expected': capture['files'][name + '.img']['sha256'],
                         'actual': hashes[name],
                         'matches': capture['files'][name + '.img']['sha256'] == hashes[name]}
                   for name in names}
    properties = {}
    for key in ('sys.boot_completed', 'ro.product.device', 'ro.boot.slot_suffix',
                'ro.build.version.incremental'):
        properties[key] = subprocess.check_output(adb + ['shell', 'getprop', key],
                                                 text=True, timeout=10).strip()
    root_status = subprocess.check_output(adb + ['exec-out', "su -c 'id'"],
                                          text=True, timeout=10).strip()
    data = {'test_id': args.test_id, 'all_26_match': all(x['matches'] for x in comparisons.values()),
            'partitions': comparisons, 'properties': properties, 'root_status': root_status}
    output = root / 'private/analysis' / f'partition-verification-test-{args.test_id}.json'
    if output.exists():
        raise SystemExit('Verification record exists; use a new test-id')
    output.write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({k: v for k, v in data.items() if k != 'partitions'}, indent=2))
    if not data['all_26_match']:
        raise SystemExit('Boot partition hash mismatch')


if __name__ == '__main__':
    main()
