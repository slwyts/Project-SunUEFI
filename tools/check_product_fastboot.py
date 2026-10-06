#!/usr/bin/env python3
"""Private, stock-fastboot acceptance of an already RAM-booted product image.

Never starts a device boot or writes device storage. Default has no action
commands; --reboot-after-check sends one ordinary reboot after read checks.
Archive/record binding is host provenance, not a USB identity attestation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import time
import zlib
from check_fastboot_debug import CheckFailed, parse_hash, parse_variable

ROOT = Path(__file__).resolve().parents[1]
SERIAL = 'SunUEFI-piano'
IMAGE = 'PianoUEFI-product.img'
FD = 'PianoUEFI-product.fd'
PATTERN_BYTES = 65553


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def bind_record(root, test_id):
    path = root / f'private/analysis/stage0-test-{test_id}.json'
    record = json.loads(path.read_text())
    if (record.get('profile') != 'product' or record.get('artifact') != IMAGE
            or record.get('error') or record.get('flash_commands_performed') is not False
            or record.get('fastboot_boot', {}).get('exit_code') != 0):
        raise CheckFailed('Matching successful --profile product RAM test record required')
    archive = root / f'artifacts/tests/stage0-test-{test_id}'
    manifest = json.loads((archive / 'manifest.json').read_text())
    build = json.loads((archive / 'build-ok.json').read_text())
    build_id = record.get('build_id')
    if (not isinstance(build_id, str) or not build_id
            or build.get('profile') != 'product'
            or build.get('status') != 'successful build, matching inputs and outputs'
            or build.get('build_id') != build_id or manifest.get('build_id') != build_id
            or record.get('build_inputs') != build.get('inputs')
            or manifest.get('build_inputs') != build.get('inputs')
            or not isinstance(build.get('inputs', {}).get('sha256'), str)):
        raise CheckFailed('Product test/build/package provenance mismatch')
    hashes = {}
    for name in (IMAGE, FD, 'BootShim.bin'):
        metadata = manifest.get('files', {}).get(name, {})
        source = archive / name
        digest = sha(source)
        if digest != metadata.get('sha256') or source.stat().st_size != metadata.get('bytes'):
            raise CheckFailed('Archived product file hash/size mismatch: ' + name)
        if name != IMAGE and digest != build.get('outputs', {}).get(name):
            raise CheckFailed('Archived file belongs to another build: ' + name)
        hashes[name] = digest
    if hashes[IMAGE] != record.get('image_sha256') or hashes[FD] != record.get('fd_sha256'):
        raise CheckFailed('RAM boot record does not match the archived product image')
    return {'test_record': str(path), 'test_record_sha256': sha(path),
            'archive': str(archive), 'build_id': build_id,
            'build_inputs': build['inputs'], 'files': hashes,
            'candidate_status': manifest.get('status'),
            'identity_scope': 'host-successful-RAM-transfer-and-sealed-archive-binding',
            'usb_protocol_build_identity_proved': False}


def baseline(root):
    folder = root / 'private/captures/2026-10-03-piano'
    image = folder / 'xbl_config_a.img'
    capture = json.loads((folder / 'manifest.json').read_text())
    expected = capture['files'][image.name]['sha256']
    if sha(image) != expected or image.stat().st_size != 524288:
        raise CheckFailed('Original xbl_config_a backup provenance changed')
    return image, expected


def verify_bmp(data, width, height, size, checksum):
    if (len(data) != size or zlib.crc32(data) != checksum or len(data) < 54
            or data[:2] != b'BM'):
        raise CheckFailed('Frozen GOP BMP bytes/CRC/header mismatch')
    file_size, offset = struct.unpack_from('<I', data, 2)[0], struct.unpack_from('<I', data, 10)[0]
    dib, actual_width, actual_height, planes, bits, compression, pixels = struct.unpack_from('<IiiHHII', data, 14)
    pitch = (width * 3 + 3) & ~3
    if (not 0 < width <= 4096 or not 0 < height <= 4096 or width * height > 8 * 1024 * 1024
            or file_size != size or offset != 54 or dib != 40
            or actual_width != width or actual_height != height or planes != 1
            or bits != 24 or compression != 0 or pixels != pitch * height
            or size != 54 + pitch * height):
        raise CheckFailed('GOP BMP geometry/24-bit bottom-up layout mismatch')
    return {'width': width, 'height': height, 'bytes': size,
            'crc32': f'{checksum:08X}', 'sha256': hashlib.sha256(data).hexdigest(),
            'format': '24-bit BGR bottom-up BMP', 'metadata_matches_bmp': True,
            'native_piano_resolution': (width, height) == (3200, 2136)}


def text_output(value):
    return value.decode(errors='replace') if isinstance(value, bytes) else (value or '')


def run_check(root, test_id, phase='simpleinit', wait_seconds=60, command_timeout=45,
              reboot_after_check=False, navigate=None):
    if (test_id < 1 or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', phase)
            or not 1 <= wait_seconds <= 60 or not 1 <= command_timeout <= 60
            or navigate not in (None, 'setup', 'shell', 'simpleinit')):
        raise ValueError('Invalid test id, phase or bounded timeout')
    out = root / f'private/analysis/product-fastboot-test-{test_id}' / phase
    out.mkdir(parents=True, exist_ok=False)
    result = {'schema': 1, 'test_id': test_id, 'phase_label': phase,
              'status': 'RUNNING', 'serial': SERIAL, 'transport': 'stock-fastboot-cli',
              'commands': [], 'persistent_write_commands': False,
              'action_commands_requested': bool(reboot_after_check or navigate),
              'navigation_requested': navigate,
              'navigation_request_acknowledged': False,
              'target_ui_proved': False,
              'reboot_command_acknowledged': False, 'owner_retirement_proved': False,
              'android_recovery_proved': False, 'all_ui_acceptance_proved': False}

    def command(*arguments):
        started = time.monotonic()
        try:
            process = subprocess.run(['fastboot', '-s', SERIAL, *arguments],
                                     capture_output=True, text=True, timeout=command_timeout)
            row = {'arguments': list(arguments), 'exit_code': process.returncode,
                   'output': process.stdout + process.stderr}
        except subprocess.TimeoutExpired as error:
            row = {'arguments': list(arguments), 'exit_code': None, 'timed_out': True,
                   'output': text_output(error.stdout) + text_output(error.stderr)}
            row['elapsed_seconds'] = round(time.monotonic() - started, 3)
            result['commands'].append(row)
            raise CheckFailed('Stock fastboot command timed out: ' + arguments[0]) from error
        row['elapsed_seconds'] = round(time.monotonic() - started, 3)
        result['commands'].append(row)
        if row['exit_code']:
            raise CheckFailed('Stock fastboot command failed: ' + arguments[0])
        return row['output']

    def variable(name):
        return parse_variable(command('getvar', name), name)

    def hex_variable(name):
        value = variable(name)
        if not re.fullmatch(r'(?:0x)?[0-9A-Fa-f]+', value):
            raise CheckFailed('Invalid hexadecimal metadata: ' + name)
        return int(value, 16)

    def ramlog(label):
        command('oem', 'ramlog')
        size = hex_variable('SunUEFI:log-size')
        checksum = hex_variable('SunUEFI:log-crc32')
        generation = hex_variable('SunUEFI:log-generation')
        if not 0 < size <= 262144 or not 0 <= checksum <= 0xffffffff or not generation:
            raise CheckFailed('Invalid frozen RAM log metadata')
        target = out / f'ramlog-{label}.bin'
        command('get_staged', str(target.resolve()))
        data = target.read_bytes()
        if len(data) != size or zlib.crc32(data) != checksum:
            raise CheckFailed('Frozen RAM log size/CRC32 mismatch')
        (out / f'ramlog-{label}.txt').write_text(data.decode(errors='replace'))
        return {'bytes': size, 'crc32': f'{checksum:08X}', 'generation': generation,
                'sha256': hashlib.sha256(data).hexdigest(), 'source': 'CRC-verified immutable console-tail snapshot',
                'session_marker_found': b'SUNUEFI_RAMLOG_BEGIN' in data,
                'product_core_ready_found': b'PIANO_PRODUCT_CORE_READY' in data,
                'product_usb_start_found': b'PIANO_PRODUCT_USB_START' in data,
                'scope': 'bounded tail; earlier session text may have wrapped',
                'file': str(target)}

    try:
        result['binding'] = bind_record(root, test_id)
        original, expected_hash = baseline(root)
        # Preserve the exact local association even if a later recovery report
        # updates the runner record. No private output enters public artifacts.
        (out / 'ram-boot-record.json').write_bytes(Path(result['binding']['test_record']).read_bytes())
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                inventory = subprocess.run(['fastboot', 'devices'], capture_output=True,
                                           text=True, timeout=min(5, remaining))
            except subprocess.TimeoutExpired:
                continue
            if inventory.returncode == 0 and any(line.split()[:2] == [SERIAL, 'fastboot']
                                                 for line in inventory.stdout.splitlines()):
                break
            time.sleep(min(1, max(0, deadline - time.monotonic())))
        else:
            raise CheckFailed('Product USB fastboot did not enumerate within bounded wait')
        if time.monotonic() >= deadline:
            raise CheckFailed('Product USB fastboot did not enumerate within bounded wait')
        variables = {}
        for name, expected in (('product', 'piano-sunuefi'), ('version', '0.4'),
                               ('serialno', SERIAL), ('is-userspace', 'no'),
                               ('storage-policy', 'no-persistent-writes')):
            actual = variable(name)
            variables[name] = actual
            if actual != expected:
                raise CheckFailed('Unexpected product fastboot variable: ' + name)
        download_limit = hex_variable('max-download-size')
        fetch_limit = hex_variable('max-fetch-size')
        partition_size = hex_variable('partition-size:xbl_config_a')
        if download_limit != 67108864 or fetch_limit != 65536 or partition_size != original.stat().st_size:
            raise CheckFailed('Unexpected download/fetch/partition bounds')
        variables.update({'max-download-size': download_limit, 'max-fetch-size': fetch_limit,
                          'partition-size:xbl_config_a': partition_size,
                          'SunUEFI:ram-boot': variable('SunUEFI:ram-boot'),
                          'SunUEFI:usb-state': variable('SunUEFI:usb-state')})
        if variables['SunUEFI:ram-boot'] != 'disabled':
            raise CheckFailed('First product acceptance expected an unbound RAM-boot backend')
        if not re.fullmatch(r'configured-speed-[SFH]', variables['SunUEFI:usb-state']):
            raise CheckFailed('Unexpected configured USB state')
        command('getvar', 'all')
        status_output = command('oem', 'status')
        if any(row not in status_output for row in ('storage-policy:no-persistent-writes', 'transport:owned-DMA-SMMU-bulk')):
            raise CheckFailed('Unexpected product status report')
        result['variables'] = variables
        if navigate is not None:
            command('oem', navigate)
            result['navigation_request_acknowledged'] = True
            # The request is a cooperative CPU latch. Re-query the real USB
            # service, then keep the later screenshot for Root inspection;
            # an ACK or phase label does not attest the target UI.
            state = variable('SunUEFI:usb-state')
            if not re.fullmatch(r'configured-speed-[SFH]', state):
                raise CheckFailed('USB state changed unexpectedly after navigation request')
            result['usb_state_after_navigation'] = state
            command('oem', 'status')
        pattern = bytes((index * 73 + index // 251 + 19) & 255 for index in range(PATTERN_BYTES))
        source = out / 'ram-pattern.bin'
        source.write_bytes(pattern)
        command('stage', str(source.resolve()))
        pattern_hash = hashlib.sha256(pattern).hexdigest()
        if parse_hash(command('oem', 'sha256')) != pattern_hash:
            raise CheckFailed('RAM stage SHA256 mismatch')
        uploaded = out / 'ram-upload.bin'
        command('get_staged', str(uploaded.resolve()))
        if uploaded.read_bytes() != pattern:
            raise CheckFailed('RAM stage/upload byte comparison failed')
        result['ram_roundtrip'] = {'bytes': len(pattern), 'sha256': pattern_hash, 'byte_equal': True}
        result['ramlog_initial'] = ramlog('initial')
        command('oem', 'screenshot')
        width = hex_variable('SunUEFI:screen-width')
        height = hex_variable('SunUEFI:screen-height')
        size = hex_variable('SunUEFI:screen-size')
        checksum = hex_variable('SunUEFI:screen-crc32')
        generation = hex_variable('SunUEFI:screen-generation')
        if not generation or size > download_limit:
            raise CheckFailed('Invalid frozen screen generation/bounds')
        screen = out / 'screen.bmp'
        command('get_staged', str(screen.resolve()))
        result['screenshot'] = {**verify_bmp(screen.read_bytes(), width, height, size, checksum),
                                'generation': generation, 'file': str(screen),
                                'ui_label_is_operator_assigned': True, 'ui_contents_verified': False}
        fetched = out / 'xbl_config_a.img'
        command('fetch', 'xbl_config_a', str(fetched.resolve()))
        if fetched.read_bytes() != original.read_bytes() or sha(fetched) != expected_hash:
            raise CheckFailed('Readonly fetch differs from the original PC backup')
        result['readonly_fetch'] = {'partition': 'xbl_config_a', 'bytes': fetched.stat().st_size,
                                    'sha256': expected_hash, 'byte_equal_original_backup': True,
                                    'max_fetch_size': fetch_limit, 'file': str(fetched)}
        command('oem', 'status')
        result['ramlog_after_fetch'] = ramlog('after-fetch')
        command('oem', 'discard')
        result['status'] = 'VERIFIED_PRODUCT_STOCK_FASTBOOT_READ_CHECKS'
        result['read_checks_verified'] = True
        if reboot_after_check:
            command('reboot')
            result['reboot_command_acknowledged'] = True
            result['status'] = 'READ_CHECKS_VERIFIED_REBOOT_ACK_ONLY'
    except Exception as error:
        result['status'] = 'FAILED'
        result['error'] = str(error)
        result.setdefault('read_checks_verified', False)
        raise
    finally:
        temporary = out / 'manifest.json.tmp'
        temporary.write_text(json.dumps(result, indent=2) + '\n')
        temporary.replace(out / 'manifest.json')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-id', required=True, type=int)
    parser.add_argument('--phase', default='simpleinit', help='Private evidence label; does not navigate the UI')
    parser.add_argument('--navigate', choices=('setup', 'shell', 'simpleinit'),
                        help='Request a UI with OEM after identity/read checks; ACK is not UI proof')
    parser.add_argument('--wait-seconds', type=int, default=60)
    parser.add_argument('--command-timeout', type=int, default=45)
    parser.add_argument('--reboot-after-check', action='store_true',
                        help='Send ordinary reboot only after checks; ACK is not retirement/recovery proof')
    args = parser.parse_args()
    if args.test_id < 1 or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', args.phase) or not 1 <= args.wait_seconds <= 60 or not 1 <= args.command_timeout <= 60:
        parser.error('Invalid test id, phase or timeout (each timeout must be 1..60 seconds)')
    try:
        result = run_check(ROOT, args.test_id, args.phase, args.wait_seconds,
                           args.command_timeout, args.reboot_after_check, args.navigate)
    except Exception as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'commands'}, indent=2))


if __name__ == '__main__':
    main()
