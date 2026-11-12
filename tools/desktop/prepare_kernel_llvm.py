#!/usr/bin/env python3
"""Prepare the fixed official LLVM release for the kernel and external modules."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / 'config/kernel-llvm.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare():
    policy = json.loads(POLICY.read_text())
    row = policy['platforms'].get(platform.machine())
    if row is None:
        raise ValueError('No kernel LLVM build for this host architecture')
    cache = ROOT / 'build/toolchains'
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / row['directory']
    with (cache / (row['directory'] + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        archive = cache / row['archive']
        if not archive.is_file():
            partial = archive.with_suffix(archive.suffix + '.part')
            for attempt in range(4):
                try:
                    print('Downloading official LLVM ' + policy['version'], flush=True)
                    with urllib.request.urlopen(policy['download_base'] + row['archive'], timeout=60) as source, partial.open('wb') as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
                    if partial.stat().st_size != row['bytes'] or digest(partial) != row['sha256']:
                        raise ValueError('Kernel LLVM archive size or checksum differs')
                    partial.replace(archive)
                    break
                except (OSError, ValueError):
                    if attempt == 3:
                        raise
                    time.sleep(2 ** attempt)
        if archive.stat().st_size != row['bytes'] or digest(archive) != row['sha256']:
            raise ValueError('Cached kernel LLVM archive checksum differs')
        marker = destination / '.sunuefi-source.json'
        names = ('clang', 'ld.lld', 'llvm-ar', 'llvm-nm', 'llvm-link',
                 'llvm-objcopy', 'llvm-objdump', 'llvm-readelf', 'llvm-strip')
        archive_identity = {'sha256': row['sha256'], 'bytes': row['bytes']}
        previous = json.loads(marker.read_text()) if marker.is_file() else None
        if previous is not None and previous.get('archive') != archive_identity:
            if any(digest(destination / 'bin' / name) != value
                   for name, value in previous['tools'].items()):
                raise ValueError('Prepared kernel LLVM tools changed')
            previous = None
        if previous is None:
            subprocess.run(['tar', '--use-compress-program=zstd -d --long=30',
                            '-xf', str(archive), '-C', str(cache),
                            *[row['directory'] + '/' + name for name in policy['members']]], check=True)
            record = {'archive': archive_identity, 'policy_sha256': digest(POLICY),
                      'tools': {name: digest(destination / 'bin' / name) for name in names}}
            marker.write_text(json.dumps(record, indent=2) + '\n')
        record = json.loads(marker.read_text())
        if any(name not in record['tools'] or
                digest(destination / 'bin' / name) != record['tools'][name] for name in names):
            raise ValueError('Prepared kernel LLVM tools changed')
        if record['policy_sha256'] != digest(POLICY):
            record['policy_sha256'] = digest(POLICY)
            marker.write_text(json.dumps(record, indent=2) + '\n')
        version = subprocess.check_output([destination / 'bin/clang', '--version'], text=True).splitlines()[0]
        if not version.startswith('clang version ' + policy['version'] + ' '):
            raise ValueError('Prepared kernel LLVM version differs')
    return destination


def environment(base=None):
    env = dict(os.environ if base is None else base)
    root = prepare()
    env['PATH'] = str(root / 'bin') + os.pathsep + env.get('PATH', '')
    return env
