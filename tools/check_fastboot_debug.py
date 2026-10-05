#!/usr/bin/env python3
"""Exercise SunUEFI USB debugging using the stock fastboot CLI only.

RAM stage/upload and fixed diagnostic commands only. No flash, erase, boot,
reboot, arbitrary OEM, storage access or custom USB transport.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import zlib

SERIAL = 'SunUEFI-piano'
PRODUCT = 'piano-sunuefi'
PATTERN_BYTES = 65553  # Crosses 64 KiB and ends with a short bulk packet.


class CheckFailed(RuntimeError):
    pass


def parse_variable(output, name):
    values = re.findall(r'(?m)^(?:\(bootloader\)\s*)?' + re.escape(name) + r':\s*(\S+)\s*$', output)
    if len(values) != 1:
        raise CheckFailed('Missing or ambiguous fastboot variable ' + name)
    return values[0]


def parse_hash(output):
    chunks = re.findall(r'(?m)^\(bootloader\)\s*([0-9a-fA-F]{32})\s*$', output)
    if len(chunks) != 2:
        raise CheckFailed('Expected exactly two SHA256 INFO rows')
    return ''.join(chunks).lower()


def run_check(out, wait_seconds=180):
    out.mkdir(parents=True, exist_ok=False)
    result = {'status': 'RUNNING', 'serial': SERIAL, 'host_transport': 'stock-fastboot-cli',
              'persistent_write_commands': False, 'commands': []}

    def command(*arguments):
        started = time.monotonic()
        try:
            process = subprocess.run(['fastboot', '-s', SERIAL, *arguments],
                                     capture_output=True, text=True, timeout=12)
            row = dict(arguments=list(arguments), exit_code=process.returncode,
                       output=process.stdout + process.stderr,
                       elapsed_seconds=round(time.monotonic() - started, 3))
        except subprocess.TimeoutExpired as exc:
            row = dict(arguments=list(arguments), exit_code=None, output='TIMEOUT',
                       elapsed_seconds=round(time.monotonic() - started, 3))
            result['commands'].append(row)
            raise CheckFailed('fastboot command timed out: ' + arguments[0]) from exc
        result['commands'].append(row)
        if row['exit_code']:
            raise CheckFailed('fastboot command failed: ' + arguments[0] + ': ' + row['output'])
        return row['output']

    def variable(name):
        return parse_variable(command('getvar', name), name)

    try:
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            devices = subprocess.run(['fastboot', 'devices'], capture_output=True,
                                     text=True, timeout=5)
            if devices.returncode == 0 and any(line.split()[:2] == [SERIAL, 'fastboot']
                                              for line in devices.stdout.splitlines()):
                break
            time.sleep(1)
        else:
            raise CheckFailed('SunUEFI fastboot did not enumerate before deadline')
        if variable('product') != PRODUCT or variable('version') != '0.4':
            raise CheckFailed('Unexpected firmware product/version')
        command('getvar', 'all')
        command('oem', 'status')
        result['usb_state'] = variable('SunUEFI:usb-state')
        # Deterministic nonzero pattern: no device or user data is downloaded.
        pattern = bytes((index * 73 + index // 251 + 19) & 255 for index in range(PATTERN_BYTES))
        input_file = out / 'ram-pattern.bin'
        input_file.write_bytes(pattern)
        command('stage', str(input_file.resolve()))
        actual_hash = parse_hash(command('oem', 'sha256'))
        expected_hash = hashlib.sha256(pattern).hexdigest()
        if actual_hash != expected_hash:
            raise CheckFailed('Firmware RAM download SHA256 mismatch')
        upload_file = out / 'ram-upload.bin'
        command('get_staged', str(upload_file.resolve()))
        if not upload_file.is_file() or upload_file.read_bytes() != pattern:
            raise CheckFailed('Standard upload did not return the exact RAM download')
        result['ram_roundtrip'] = {'bytes': len(pattern), 'sha256': expected_hash, 'byte_equal': True}
        command('oem', 'ramlog')
        length = int(variable('SunUEFI:log-size'), 16)
        checksum = int(variable('SunUEFI:log-crc32'), 16)
        generation = int(variable('SunUEFI:log-generation'), 16)
        if not 0 < length <= 65536 or not generation or not 0 <= checksum <= 0xffffffff:
            raise CheckFailed('Invalid frozen RAM log metadata')
        log_file = out / 'ramlog.bin'
        command('get_staged', str(log_file.resolve()))
        if not log_file.is_file():
            raise CheckFailed('RAM log upload file missing')
        log = log_file.read_bytes()
        if len(log) != length or zlib.crc32(log) != checksum:
            raise CheckFailed('Frozen RAM log size/CRC32 mismatch')
        (out / 'ramlog.txt').write_text(log.decode(errors='replace'))
        result['ram_log'] = {'bytes': length, 'crc32': f'{checksum:08X}', 'generation': generation,
                             'sha256': hashlib.sha256(log).hexdigest(),
                             'current_session_marker': b'SUNUEFI_RAMLOG_BEGIN' in log}
        command('oem', 'discard')
        result['status'] = 'VERIFIED_FASTBOOT_RAM_AND_LOG_ROUNDTRIP'
        result['debug_verified'] = True
    except Exception as exc:
        result['status'] = 'FAILED'
        result['error'] = str(exc)
        result['debug_verified'] = False
        raise
    finally:
        (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--test-id', type=int, required=True)
    ap.add_argument('--wait-seconds', type=int, default=180)
    args = ap.parse_args()
    if args.test_id < 1 or not 1 <= args.wait_seconds <= 300:
        ap.error('Invalid test ID or wait duration')
    root = Path(__file__).resolve().parents[1]
    out = root / f'private/analysis/fastboot-debug-host-test-{args.test_id}'
    result = run_check(out, args.wait_seconds)
    print(json.dumps({key: value for key, value in result.items() if key != 'commands'}, indent=2))


if __name__ == '__main__':
    main()
