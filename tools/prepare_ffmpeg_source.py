#!/usr/bin/env python3
"""Stage the fixed upstream FFmpeg source and the distro-neutral BSP patch."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SERIES = ROOT / 'linux/bsp/patches/ffmpeg/series.json'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(output, apply_patches=True, series=SERIES):
    output = Path(output).resolve()
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        raise ValueError('Use a new project build directory; existing source edits are preserved')
    series = Path(series).resolve()
    record = json.loads(series.read_text())
    origin = record['source']
    if 'archives' in origin:
        archive = next(row for row in origin['archives'] if row['filename'].endswith('.orig.tar.xz'))
        url = origin['dsc_url'].rsplit('/', 1)[0] + '/' + archive['filename']
    else:
        archive = origin
        url = origin['url']
    cache = ROOT / 'build/source-cache' / ('ffmpeg-' + record['upstream_version'] + '-source') / archive['filename']
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        for attempt in range(3):
            try:
                with tempfile.TemporaryDirectory(prefix='download-', dir=cache.parent) as temporary:
                    partial = Path(temporary) / cache.name
                    with urllib.request.urlopen(url, timeout=60) as response, partial.open('wb') as target:
                        shutil.copyfileobj(response, target, 1024 * 1024)
                    if partial.stat().st_size != archive['size'] or sha(partial) != archive['sha256']:
                        raise ValueError('FFmpeg upstream archive checksum mismatch')
                    partial.replace(cache)
                break
            except (urllib.error.URLError, http.client.HTTPException, TimeoutError, ConnectionError) as error:
                if (isinstance(error, urllib.error.HTTPError) and
                        error.code not in (408, 429, 500, 502, 503, 504)) or attempt == 2:
                    raise
                wait = 15 * (attempt + 1)
                print(f'FFmpeg source download failed: {error}; retrying in {wait}s '
                      f'({attempt + 2}/3).', flush=True)
                time.sleep(wait)
    if cache.stat().st_size != archive['size'] or sha(cache) != archive['sha256']:
        raise ValueError('FFmpeg upstream archive checksum mismatch')
    patches = []
    with tempfile.TemporaryDirectory(prefix='ffmpeg-source-', dir=ROOT / 'build') as temporary:
        with tarfile.open(cache, 'r:xz') as stream:
            stream.extractall(temporary, filter='data')
        shutil.copytree(Path(temporary) / ('ffmpeg-' + record['upstream_version']), output)
    for row in record['series']:
        patch = (series.parent / row['path']).resolve()
        if sha(patch) != row['sha256']:
            raise ValueError('FFmpeg BSP patch checksum mismatch: ' + row['path'])
        if apply_patches:
            subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', patch],
                           cwd=output, check=True)
        patches.append({'path': str(patch.relative_to(ROOT)), 'sha256': row['sha256']})
    source = {'component': record['component'], 'upstream_version': record['upstream_version'],
              'local_revision': record['local_revision'], 'archive_url': url,
              'archive_sha256': archive['sha256'], 'patches': patches,
              'debian_reference': record.get('debian_reference'), 'patches_applied': apply_patches,
              'packages_built': False}
    (output / 'SOURCE.json').write_text(json.dumps(source, indent=2) + '\n')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--series', type=Path, default=SERIES)
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args.output, series=args.series), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
