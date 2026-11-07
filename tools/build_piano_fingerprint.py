#!/usr/bin/env python3
"""Build the native Goodix client against fixed, public Qualcomm sources."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / 'linux/userspace/fingerprint'
CLIENT_SOURCES = [CLIENT / 'piano-goodix-protocol.c', CLIENT / 'piano-goodix.c']
RUNTIME_PATHS = {
    'usr/local/sbin/piano-fingerprint-ctl',
    'usr/share/piano-provenance/fingerprint.json',
    *{f'usr/share/licenses/piano-fingerprint/{name}.txt'
      for name in ('quic-teec', 'minkipc', 'qcbor')},
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def source_inputs():
    return {
        'config_sha256': digest(ROOT / 'config/fingerprint.json'),
        'client_sources_sha256': {path.name: digest(path) for path in
                                  [*CLIENT_SOURCES, CLIENT / 'piano-goodix-protocol.h']},
        'builder_sha256': digest(Path(__file__)),
        'firmware_manifest_sha256': digest(
            ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json'),
    }


def source(name, record):
    path = ROOT / 'build/source-cache' / name
    fresh = not path.exists()
    if fresh:
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout',
                        record['url'], path], check=True)
    if not fresh and subprocess.check_output(
            ['git', '-C', path, 'status', '--porcelain', '--untracked-files=no'],
            text=True):
        raise ValueError('Fingerprint dependency has local changes: ' + name)
    if subprocess.run(['git', '-C', path, 'cat-file', '-e', record['commit']],
                      capture_output=True).returncode:
        subprocess.run(['git', '-C', path, 'fetch', '--depth=1', 'origin',
                        record['commit']], check=True)
    subprocess.run(['git', '-C', path, 'checkout', '--detach', record['commit']],
                   check=True)
    return path


def build(output, flags, emulator=None):
    inputs = source_inputs()
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())
    dependencies = {name: source(name, record)
                    for name, record in cfg['sources'].items()}
    qtee, mink, qcbor = (dependencies[name] for name in ('quic-teec', 'minkipc', 'qcbor'))
    includes = [qtee / 'libqcomtee/include', qtee / 'libqcomtee/src',
                mink / 'libminkadaptor/include', mink / 'libminkadaptor/src',
                qcbor / 'inc']
    originals = [
        qtee / 'libqcomtee/src/qcomtee_object.c',
        qtee / 'libqcomtee/src/objects/credentials_obj.c',
        qtee / 'libqcomtee/src/objects/mem_obj.c',
        mink / 'libminkadaptor/src/mink_adaptor.c',
        mink / 'libminkadaptor/src/supplicant.c',
        mink / 'libminkadaptor/src/syscall.S',
        *[qcbor / 'src' / name for name in (
            'UsefulBuf.c', 'qcbor_encode.c', 'qcbor_decode.c',
            'qcbor_err_to_str.c', 'ieee754.c')],
        *CLIENT_SOURCES,
    ]
    work = output / 'fingerprint'
    work.mkdir()
    objects = []
    for index, original in enumerate(originals):
        obj = work / (str(index) + '.o')
        command = [*flags, '-D_GNU_SOURCE', '-DUSE_QCBOR',
                   *['-I' + str(p) for p in includes],
                   '-c', original, '-o', obj]
        # Imported Qualcomm sources retain their own warning policy.
        if original not in CLIENT_SOURCES:
            command = [arg for arg in command if arg != '-Werror']
        subprocess.run(command, check=True)
        objects.append(obj)
    binary = work / 'piano-fingerprint-ctl'
    subprocess.run([*flags, '-static', *objects, '-pthread', '-lm', '-o', binary], check=True)
    subprocess.run(([emulator] if emulator else []) + [str(binary), '--help'], check=True)
    files = {}
    for name, path, mode in [
            ('usr/local/sbin/piano-fingerprint-ctl', binary, 0o755),
            *[(f'usr/share/licenses/piano-fingerprint/{name}.txt',
               dependencies[name] / record['license'], 0o644)
              for name, record in cfg['sources'].items()]]:
        target = work / Path(name).name
        if path != target:
            shutil.copy2(path, target)
        files[name] = {'file': str(target.relative_to(output)),
                       'bytes': target.stat().st_size, 'sha256': digest(target), 'mode': mode}
    record = {
        'inputs': inputs,
        'sources': cfg['sources'],
        'client_source': 'linux/userspace/fingerprint/piano-goodix.c',
        'client_sha256': digest(CLIENT / 'piano-goodix.c'),
        'source_files': {str(p.relative_to(ROOT)): digest(p) for p in originals},
        'firmware': json.loads(
            (ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json').read_text())['files'],
        'operations': ['info', 'initialize', 'prepare-auth', 'watch', 'list', 'verify'],
        'enrollment_supported': False,
    }
    if source_inputs() != inputs:
        raise ValueError('Fingerprint sources changed during compilation')
    provenance = work / 'fingerprint.json'
    provenance.write_text(json.dumps(record, indent=2) + '\n')
    files['usr/share/piano-provenance/fingerprint.json'] = {
        'file': str(provenance.relative_to(output)), 'bytes': provenance.stat().st_size,
        'sha256': digest(provenance), 'mode': 0o644}
    return files, record
