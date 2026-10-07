#!/usr/bin/env python3
"""Get or check the fixed build sources. Does not reset edits or touch a device."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOP = ('upstream/linux-piano', 'upstream/Mu-Silicium', 'upstream/simple-init',
       'upstream/debian-piano-current', 'upstream/piano-firmware-current',
       'upstream/piano-mesa-current', 'upstream/dtc',
       'upstream/audioreach-topology', 'upstream/v4l2loopback')
# Deliberately do not recurse into upstream test suites and fuzz corpora.
NESTED = {
    'upstream/Mu-Silicium': ('Binaries', 'Common/Mu', 'Common/Mu_OEM_Sample',
        'Mu_Basecore', 'Silicium-ACPI',
        'Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl'),
    'upstream/Mu-Silicium/Mu_Basecore': (
        'BaseTools/Source/C/BrotliCompress/brotli',
        'MdeModulePkg/Library/BrotliCustomDecompressLib/brotli',
        'MdeModulePkg/Universal/RegularExpressionDxe/oniguruma',
        'MdePkg/Library/BaseFdtLib/libfdt', 'MdePkg/Library/MipiSysTLib/mipisyst'),
    'upstream/simple-init': ('libs/freetype',),
}


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def expected(root):
    lock = json.loads((root / 'sources.lock.json').read_text())
    rows = {'upstream/' + name: value['commit'] for name, value in lock.items()}
    rows['upstream/linux-piano'] = json.loads((root / 'linux/kernel-profiles.json').read_text())['profiles']['stable']['commit']
    return rows


def check(root):
    failures = []
    for name, commit in expected(root).items():
        path = root / name
        try:
            # A missing nested repo must not accidentally resolve its parent.
            top = Path(git(path, 'rev-parse', '--show-toplevel')).resolve()
            if top != path.resolve() or git(path, 'rev-parse', 'HEAD') != commit:
                failures.append(name + ': expected ' + commit)
        except (OSError, subprocess.CalledProcessError):
            failures.append(name + ': missing checkout')
    if failures:
        raise ValueError('\n'.join(failures))
    return len(expected(root))


def prepare(root):
    # No --force or --remote: Git rejects changes that cannot be preserved and
    # checks out the gitlink, not a moving branch tip.
    subprocess.run(['git', '-C', str(root), 'submodule', 'update', '--init',
                    '--depth', '1', '--', *TOP], check=True)
    for parent, children in NESTED.items():
        subprocess.run(['git', '-C', str(root / parent), 'submodule', 'update',
                        '--init', '--depth', '1', '--', *children], check=True)
    return check(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Verify only; no network or checkout changes')
    args = parser.parse_args()
    try:
        count = check(ROOT) if args.check else prepare(ROOT)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
    print(f'{count} fixed source checkouts verified. Applied overlays are checked by the product builder.')


if __name__ == '__main__':
    main()
