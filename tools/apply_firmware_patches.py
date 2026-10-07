#!/usr/bin/env python3
"""Apply the recorded firmware patches; keep strict prepare/verify checks."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def apply(root=ROOT):
    folder = root / 'patches/firmware'
    manifest = json.loads((folder / 'series.json').read_text())
    if manifest.get('schema_version') != 1:
        raise ValueError('Unsupported firmware patch series')
    for row in manifest['patches']:
        repo = root / row['repository']
        patch = folder / row['patch']
        if not patch.resolve().is_relative_to(folder.resolve()):
            raise ValueError('Patch outside series directory')
        if hashlib.sha256(patch.read_bytes()).hexdigest() != row['sha256']:
            raise ValueError('Patch hash mismatch: ' + patch.name)
        head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
        if head != row['base_commit']:
            raise ValueError('Unexpected upstream commit: ' + row['repository'])
        command = ['git', '-C', str(repo), 'apply']
        forward = subprocess.run([*command, '--check', str(patch)], capture_output=True)
        if forward.returncode == 0:
            subprocess.run([*command, str(patch)], check=True)
            state = 'applied'
        else:
            reverse = subprocess.run([*command, '--reverse', '--check', str(patch)], capture_output=True)
            if reverse.returncode:
                raise ValueError('Patch conflicts with upstream edits: ' + patch.name)
            state = 'already applied'
        print(patch.name + ': ' + state, flush=True)


if __name__ == '__main__':
    try:
        apply()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
