#!/usr/bin/env python3
"""Read-only, allowlisted ADB capture. All output is written on the host."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

PROPERTIES = (
    'ro.product.model', 'ro.product.device', 'ro.product.board',
    'ro.board.platform', 'ro.soc.model', 'ro.soc.manufacturer',
    'ro.boot.slot_suffix', 'ro.boot.flash.locked', 'ro.boot.verifiedbootstate',
    'ro.boot.vbmeta.device_state', 'ro.build.version.release',
    'ro.build.version.sdk', 'ro.build.version.security_patch',
    'ro.build.version.incremental', 'ro.build.display.id',
)
# Excludes userdata, metadata, persist, key stores, RPMB and identity partitions.
PARTITIONS = tuple(f'{name}_{slot}' for name in (
    'boot', 'init_boot', 'vendor_boot', 'dtbo', 'vbmeta', 'vbmeta_system',
    'recovery', 'uefi', 'abl', 'xbl', 'xbl_config', 'hyp', 'devcfg',
) for slot in ('a', 'b'))
READS = {
    'uname.txt': 'uname -a',
    'iomem.txt': 'cat /proc/iomem',
    'cmdline.txt': 'cat /proc/cmdline',
    'meminfo.txt': 'cat /proc/meminfo',
    'partitions.txt': 'ls -l /dev/block/by-name',
    'live.dtb': 'cat /sys/firmware/fdt',
    'config.gz': 'cat /proc/config.gz',
    'soc.txt': "for p in soc_id family machine revision; do f=/sys/devices/soc0/$p; if test -r \"$f\"; then printf '%s=' \"$p\"; cat \"$f\"; fi; done",
    'hypervisor.txt': 'ls -l /dev/kvm /dev/gunyah /dev/gh_vm /dev/gh_ctrl /dev/gzvm 2>/dev/null; true',
}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--serial', help='Select an ADB device; not stored in output')
    ap.add_argument('--metadata-only', action='store_true')
    args = ap.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=False)
    adb = ['adb'] + (['-s', args.serial] if args.serial else [])

    def read(command, root=False, timeout=60):
        remote = 'su -c ' + shlex.quote(command) if root else command
        p = subprocess.run(adb + ['exec-out', remote], capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError(f'Read failed: {command}: {p.stderr.decode(errors="replace")}')
        return p.stdout

    props = {p: read('getprop ' + p).decode().strip() for p in PROPERTIES}
    if props['ro.product.device'] != 'piano' or props['ro.board.platform'] != 'sun':
        raise SystemExit('Device mismatch: expected piano / sun; no partition reads performed')
    if b'uid=0' not in read('id', root=True):
        raise SystemExit('Root is required for read-only firmware capture')
    manifest = {'captured_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'properties': props, 'files': {}, 'errors': {},
                'device_policy': 'allowlisted reads only; no device file/partition writes'}

    def save(name, data):
        (args.output / name).write_bytes(data)
        manifest['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

    def checkpoint():
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    save('properties.json', (json.dumps(props, indent=2) + '\n').encode())
    for name, command in READS.items():
        try:
            save(name, read(command, root=True))
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            manifest['errors'][name] = str(exc)
        checkpoint()
    if not args.metadata_only:
        for partition in PARTITIONS:
            path = '/dev/block/by-name/' + partition
            name = partition + '.img'
            try:
                size = int(read('blockdev --getsize64 ' + path, root=True))
                if size <= 0 or size > 128 * 1024 * 1024:
                    raise RuntimeError(f'Unexpected size: {size}')
                target = args.output / name
                temporary = target.with_suffix('.partial')
                with temporary.open('xb') as out:
                    p = subprocess.run(adb + ['exec-out', 'su -c ' + shlex.quote('cat ' + path)],
                                       stdout=out, stderr=subprocess.PIPE, timeout=180)
                if p.returncode:
                    raise RuntimeError(p.stderr.decode(errors='replace'))
                if temporary.stat().st_size != size:
                    raise RuntimeError('Truncated ADB partition read')
                digest = hashlib.file_digest(temporary.open('rb'), 'sha256').hexdigest()
                device_digest = read('sha256sum ' + path, root=True, timeout=180).decode().split()[0]
                if digest != device_digest:
                    raise RuntimeError('Host/device SHA-256 mismatch')
                temporary.rename(target)
                manifest['files'][name] = {'bytes': size, 'sha256': digest,
                                           'device_sha256': device_digest, 'verified': True}
                print(f'{partition}: {size // 1024 // 1024} MiB, SHA-256 verified', flush=True)
            except (RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
                manifest['errors'][name] = str(exc)
            checkpoint()
    print(f'Capture complete: {len(manifest["files"])} files; {len(manifest["errors"])} errors')
    if manifest['errors']:
        print(json.dumps(manifest['errors'], indent=2))
        raise SystemExit(1)

if __name__ == '__main__':
    main()
