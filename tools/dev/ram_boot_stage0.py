#!/usr/bin/env python3
"""Explicit RAM-only test of the locally built piano diagnostic image.

Requires a person at the tablet for power-key recovery. No flash/erase/slot commands.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
from build_integrity import validate

def command(argv, timeout=10):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr).strip()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--test-id', type=int, required=True)
    ap.add_argument('--execute', action='store_true', help='Reboot and send the image to RAM; operator must be present')
    ap.add_argument('--profile', choices=('stage0', 'probe', 'linux', 'gui', 'product'), default='stage0')
    ap.add_argument('--serial', help='Select the piano ADB/fastboot device explicitly')
    args = ap.parse_args()
    if args.test_id <= 0:
        raise SystemExit('test-id must be positive')
    root = Path(__file__).resolve().parent.parent
    out = root / 'artifacts' / args.profile
    manifest = json.loads((out / 'manifest.json').read_text())
    try:
        build_record=validate(root,args.profile,manifest)
        if args.profile=='product':
            from product_contract import validate as validate_contract,validate_build_manifest
            contract=validate_contract(json.loads((root/'config/piano-product.json').read_text()))
            validate_build_manifest(contract,manifest)
    except (ValueError,OSError,KeyError) as error:
        raise SystemExit(str(error))
    image = out / ('PianoUEFI-product.img' if args.profile=='product' else 'piano-stage0-UNTESTED.img')
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if digest != manifest['files'][image.name]['sha256']:
        raise SystemExit('Candidate hash mismatch')
    if not args.execute:
        print(f'Candidate {image}: SHA-256 {digest}. No device action performed.')
        return
    serial = args.serial
    if serial is None:
        inventory = subprocess.check_output(['adb', 'devices', '-l'], text=True, timeout=10)
        candidates = [parts[0] for line in inventory.splitlines()
                      if len(parts := line.split()) >= 2 and parts[1] == 'device'
                      and 'device:piano' in parts]
        if len(candidates) != 1:
            raise SystemExit('Expected one authorized piano; use --serial to select explicitly')
        serial = candidates[0]
    adb = ['adb', '-s', serial]
    fastboot = ['fastboot', '-s', serial]
    capture = json.loads((root / 'private/captures/2026-10-03-piano/manifest.json').read_text())
    for key, expected in {'sys.boot_completed': '1', 'ro.product.device': 'piano', 'ro.board.platform': 'sun',
                          'ro.boot.slot_suffix': capture['properties']['ro.boot.slot_suffix'],
                          'ro.build.version.incremental': capture['properties']['ro.build.version.incremental']}.items():
        code, value = command(adb + ['shell', 'getprop', key])
        if code or value != expected:
            raise SystemExit(f'Device state mismatch: {key}')
    record_path = root / f'private/analysis/stage0-test-{args.test_id}.json'
    if record_path.exists():
        raise SystemExit('Test record already exists; choose a new test-id')
    archive = root / f'artifacts/tests/stage0-test-{args.test_id}'
    archive.mkdir(parents=True, exist_ok=False)
    fd_name='PianoUEFI-product.fd' if args.profile=='product' else 'piano-stage0.fd'
    archive_names=(image.name,fd_name,'manifest.json')
    if args.profile=='product':archive_names+=('BootShim.bin','build-ok.json')
    for name in archive_names:
        shutil.copyfile(out / name, archive / name)
    if manifest.get('linux_payload'):
        # Save the exact provenance before another stable/next packaging run
        # replaces the shared linux-ram payload output.
        (archive/'linux-payload-manifest.json').write_text(json.dumps(manifest['linux_payload'],indent=2)+'\n')
    record = {'operation': 'explicit RAM-only diagnostic test', 'profile': args.profile, 'image_sha256': digest,
              'result': 'pending screen and Android recovery observation', 'flash_commands_performed': False}
    if args.profile=='product':
        record.update({'operation':'explicit RAM-only product integration test',
            'build_id':build_record['build_id'],'build_inputs':build_record['inputs'],
            'candidate_status':manifest['status'],'artifact':image.name,
            'fd_sha256':build_record['outputs'][fd_name],
            'entry_policy':'fastboot_boot; identical shared product core and feature set'})
    try:
        subprocess.run(adb + ['reboot', 'bootloader'], check=True, timeout=10)
        for _ in range(20):
            if any(line.split()[0] == serial for line in command(['fastboot', 'devices'], 5)[1].splitlines() if line.split()):
                break
            time.sleep(1)
        else:
            raise RuntimeError('Fastboot did not enumerate; manual recovery may be required')
        slot = capture['properties']['ro.boot.slot_suffix'].removeprefix('_')
        for key, expected in (('product', 'piano'), ('unlocked', 'yes'), ('current-slot', slot)):
            code, value = command(fastboot + ['getvar', key])
            record[key] = value
            if code or f'{key}: {expected}' not in value:
                raise RuntimeError(f'Fastboot mismatch: {key}')
        code, value = command(fastboot + ['boot', str(image)], 30)
        record['fastboot_boot'] = {'exit_code': code, 'response': value}
        print(value, flush=True)
        if code:
            raise RuntimeError('Fastboot reported boot failure')
    except Exception as exc:
        record['error'] = str(exc)
        # A timeout may mean the candidate is running. Only reboot an enumerated bootloader.
        if any(line.split()[0] == serial for line in command(['fastboot', 'devices'], 5)[1].splitlines() if line.split()):
            record['recovery_reboot'] = command(fastboot + ['reboot'], 15)
        raise
    finally:
        record_path.write_text(json.dumps(record, indent=2) + '\n')
    print('Image transferred to RAM. A successful UEFI/OS boot still requires observation.', flush=True)

if __name__ == '__main__':
    main()
