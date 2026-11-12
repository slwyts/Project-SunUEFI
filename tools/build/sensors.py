#!/usr/bin/env python3
"""Build the fixed Piano sensor package set through the shared package cache."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROPERTY_TYPES = 'patches/piano-sensors/0002-fix-libssc-property-types.patch'


def main(argv):
    output = Path(argv[0]).resolve() if argv else ROOT / 'build/sensors'
    selection = argv[1:]
    if selection not in ([], ['--libssc-only']):
        raise SystemExit('Usage: ./build.sh sensors [OUTPUT_DIR] [--libssc-only]')
    config = json.loads((ROOT / 'config/release.json').read_text())['sensors']
    if selection and PROPERTY_TYPES not in config['patches']:
        raise SystemExit('Select the libssc property-type patch in sensors.patches '
                         'before building libssc-only.')
    source = ROOT / config['source']
    head = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True)
    if head != config['commit'] or dirty:
        raise SystemExit('Sensors source HEAD/cleanliness mismatch; run ./build.sh sources.')
    workspace = ROOT / 'build/sensors-workspace'
    shutil.rmtree(workspace, ignore_errors=True)
    shutil.copytree(source, workspace, ignore=shutil.ignore_patterns('.git'))
    patches = []
    for name in config['patches']:
        patch = ROOT / name
        patches.append({'path': name, 'sha256': hashlib.sha256(patch.read_bytes()).hexdigest()})
        subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', patch], cwd=workspace, check=True)
    (workspace / 'SOURCE').write_text(
        json.dumps({'source_url': config['source_url'], 'source_commit': head,
                    'patches': patches}, indent=2) + '\n')
    env = os.environ.copy()
    for name in ('VIRTUAL_ENV', 'PYTHONHOME', 'PYTHONPATH'):
        env.pop(name, None)
    env['PATH'] = '/usr/sbin:/usr/bin:/sbin:/bin'
    sys.path.insert(0, str(ROOT / 'tools'))
    from desktop.package_build import run_cached
    script = workspace / 'scripts/build-sensors-debs.sh'
    if selection:
        subprocess.run(['bash', script, output, *selection], env=env, check=True)
    else:
        run_cached(output, [ROOT / 'build.sh', ROOT / 'config/build-container.json', script,
                            *[ROOT / name for name in config['patches']]],
                   ['bash', script, output], env, selection=config)
    shutil.copy2(workspace / 'SOURCE', output / 'SOURCE')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
