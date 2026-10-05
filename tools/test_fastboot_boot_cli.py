#!/usr/bin/env python3
"""Prove stock fastboot boot/stage framing against loopback TCP only.

No USB enumeration/device and no payload execution. The fake server always
rejects the boot command after recording bytes; fixtures are artificial.
"""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def run_case(binary, command, file, options=()):
    result = {'commands': [], 'download': b'', 'errors': []}
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(10)
    port = listener.getsockname()[1]

    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(5)
                assert receive(peer, 4) == b'FB01'
                peer.sendall(b'FB01')
                expected = None
                while True:
                    try:
                        size = struct.unpack('>Q', receive(peer, 8))[0]
                        assert size <= 65536, 'Unexpected large offline frame'
                        data = receive(peer, size)
                    except EOFError:
                        return
                    if expected is not None:
                        result['download'] += data
                        assert len(result['download']) <= expected
                        if len(result['download']) < expected:
                            continue
                        expected = None
                        answer = b'OKAY'
                    else:
                        text = data.decode('ascii')
                        result['commands'].append(text)
                        if text.startswith('download:'):
                            expected = int(text[9:], 16)
                            answer = f'DATA{expected:08x}'.encode()
                        elif text == 'boot':
                            answer = b'FAILoffline fixture never executes boot'
                        elif text.startswith('getvar:'):
                            values = {'max-download-size': '0x04000000', 'is-userspace': 'no',
                                      'version': '0.4', 'product': 'offline-fixture'}
                            answer = ('OKAY' + values[text[7:]]).encode() if text[7:] in values else b'FAILunknown variable'
                        else:
                            raise AssertionError('Unexpected command: ' + text)
                    peer.sendall(struct.pack('>Q', len(answer)) + answer)
        except Exception as error:
            result['errors'].append(repr(error))
        finally:
            listener.close()

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    process = subprocess.run([binary, '-s', f'tcp:127.0.0.1:{port}', *options, command, str(file)],
                             capture_output=True, text=True, timeout=12)
    worker.join(10)
    assert not worker.is_alive(), 'Offline server did not terminate'
    assert not result['errors'], result['errors']
    result['exit_code'] = process.returncode
    result['output'] = process.stdout + process.stderr
    return result


def audit(binary='fastboot'):
    version = subprocess.run([binary, '--version'], capture_output=True, text=True, check=True).stdout.strip()
    with tempfile.TemporaryDirectory(prefix='sunuefi-fastboot-cli-') as temp:
        path = Path(temp) / 'artificial.efi'
        raw = bytearray(8192)
        raw[:2] = b'MZ'
        struct.pack_into('<I', raw, 60, 128)
        raw[128:132] = b'PE\0\0'
        struct.pack_into('<H', raw, 132, 0xaa64)
        path.write_bytes(raw)
        boot = run_case(binary, 'boot', path)
        data = boot['download']
        assert boot['exit_code'] != 0 and 'boot' in boot['commands']
        assert data[:8] == b'ANDROID!' and struct.unpack_from('<I', data, 8)[0] == len(raw)
        page = struct.unpack_from('<I', data, 36)[0]
        assert page == 2048 and data[page:page + len(raw)] == raw
        android = Path(temp) / 'artificial.img'
        android.write_bytes(data)
        passthrough = run_case(binary, 'boot', android)
        assert passthrough['exit_code'] != 0 and passthrough['download'] == data and 'boot' in passthrough['commands']
        stage = run_case(binary, 'stage', path)
        assert stage['exit_code'] == 0 and 'boot' not in stage['commands'] and stage['download'] == raw
        short = Path(temp) / 'short.efi'
        short.write_bytes(raw[:512])
        reject = run_case(binary, 'boot', short)
        assert reject['exit_code'] != 0 and not reject['download'] and 'too short' in reject['output']
        versions = []
        for header in range(1, 5):
            wrapped = run_case(binary, 'boot', path, ('--header-version', str(header)))['download']
            assert wrapped[:8] == b'ANDROID!' and struct.unpack_from('<I', wrapped, 40)[0] == header
            versions.append({'version': header, 'declared_header_size': struct.unpack_from('<I', wrapped, 20 if header >= 3 else 1644)[0],
                             'signature_size': struct.unpack_from('<I', wrapped, 1580)[0] if header == 4 else None})
        return {'status': 'VERIFIED_LOCAL_CLI_FRAMING_ONLY', 'version': version,
                'transport': 'loopback TCP fake server', 'usb_used': False, 'execution': False,
                'raw_efi_boot': {'download_is_android_container': True, 'kernel_equals_raw_fixture': True,
                                 'page_bytes': page, 'bytes': len(data), 'commands': boot['commands']},
                'raw_efi_stage': {'download_is_raw': True, 'bytes': len(raw), 'commands': stage['commands']},
                'android_img_boot': {'download_is_byte_identical': True, 'bytes': len(data), 'commands': passthrough['commands']},
                'explicit_header_versions': versions,
                'short_efi_boot': {'rejected_before_download': True, 'bytes': 512}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fastboot', default='fastboot')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    result = audit(args.fastboot)
    output = json.dumps(result, indent=2) + '\n'
    if args.json:
        args.json.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
