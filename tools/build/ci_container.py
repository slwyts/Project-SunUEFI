#!/usr/bin/env python3
"""Reuse the exact local builder image between CI jobs without registry pushes."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build.cache import atomic_json, sha, source_identity

INPUTS = ('containers/Dockerfile', 'config/build-container.json',
          'tools/build_container.py', 'tools/build/ci_container.py',
          'tools/build/cache.py')


def cache_key(root=ROOT):
    identity = source_identity(root, INPUTS)
    digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    return f'builder-{platform.machine()}-{digest}'


def image_id():
    return subprocess.check_output(
        ['docker', 'image', 'inspect', '--format={{.Id}}', 'sunuefi-builder'], text=True).strip()


def prepare(load_only=False):
    directory = ROOT / 'build/ci-builder'
    directory.mkdir(parents=True, exist_ok=True)
    identity = source_identity(ROOT, INPUTS)
    archive, marker = directory / 'image.tar', directory / 'manifest.json'
    with (directory / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        record = json.loads(marker.read_text()) if marker.is_file() else None
        if (record and record.get('inputs') == identity and archive.is_file() and
                sha(archive) == record.get('archive_sha256')):
            subprocess.run(['docker', 'load', '--input', archive], check=True)
            if image_id() != record['image_id']:
                raise ValueError('Loaded builder image differs from its cache record')
        elif load_only:
            raise ValueError('Shared builder image cache is missing or changed; rerun the environment job')
        else:
            marker.unlink(missing_ok=True)
            subprocess.run([sys.executable, ROOT / 'tools/build_container.py'], cwd=ROOT, check=True)
            temporary = directory / 'image.tar.part'
            subprocess.run(['docker', 'save', '--output', temporary, 'sunuefi-builder'], check=True)
            temporary.replace(archive)
            if source_identity(ROOT, INPUTS) != identity:
                raise ValueError('Builder recipe changed while creating the image')
            record = {'inputs': identity, 'image_id': image_id(), 'archive_sha256': sha(archive)}
            atomic_json(marker, record)
    return record['image_id']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('key', 'prepare', 'load'))
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args()
    if args.operation == 'key':
        field, label, result = 'key', 'Builder image cache key', cache_key()
    else:
        field, label, result = 'image_id', 'Builder image verified', prepare(args.operation == 'load')
    if args.github_output:
        with args.github_output.open('a') as stream:
            stream.write(f'{field}={result}\n')
    print(f'{label}: {result}')


if __name__ == '__main__':
    main()
