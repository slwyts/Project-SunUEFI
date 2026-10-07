#!/usr/bin/env python3
"""Standalone launch/dependency guard; the copied installer owns all device policy."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    if sys.version_info < (3, 10):
        raise ValueError('Python 3.10+ is required')
    base = Path(__file__).resolve().parent
    record = json.loads((base / 'installer-record.json').read_text())
    installer = base / 'install_piano.py'
    expected = record['files']['install_piano.py']['sha256']
    if installer.is_symlink() or hashlib.sha256(installer.read_bytes()).hexdigest() != expected:
        raise ValueError('Installer checksum mismatch; download the artifact again')
    args = sys.argv[1:] or ['--help']
    help_only = '--help' in args or '-h' in args
    if not help_only:
        missing = [name for name in ('adb', 'fastboot') if shutil.which(name) is None]
        if missing:
            raise ValueError('Missing Android platform-tools on PATH: ' + ', '.join(missing))
        # Skip option values so a serial/output literally named "plan" is safe.
        operation, skip = 'inspect', False
        for arg in args:
            if skip:
                skip = False
            elif arg in ('--serial', '--bundle', '--plan', '--output'):
                skip = True
            elif arg in ('inspect', 'plan', 'apply'):
                operation = arg
        if record.get('bundle') not in (None, 'bundle'):
            raise ValueError('Invalid bundled-image location')
        explicit_bundle = any(arg == '--bundle' or arg.startswith('--bundle=') for arg in args)
        if operation in ('plan', 'apply') and record.get('bundle') and not explicit_bundle:
            args += ['--bundle', str(base / 'bundle')]
    # Keep caller cwd: relative --plan/--output paths belong to the caller.
    return subprocess.call([sys.executable, str(installer), *args])


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        raise SystemExit(str(error))
