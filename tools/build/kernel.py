#!/usr/bin/env python3
"""Reuse the recorded kernel source worktree and rebuild the product kernel."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main(argv):
    manifest = ROOT / 'build/release/source-manifest.json'
    record = __import__('json').loads(manifest.read_text())
    return subprocess.run(
        ['python3', 'tools/build_piano_full_kernel.py', '--worktree', record['worktree'],
         '--commit', record['actual_commit'], '--root', 'LABEL=PIANOROOT', '--rebuild',
         '--build-dir', 'build/kernels/release', '--artifacts', 'artifacts/kernels/release',
         *argv], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
