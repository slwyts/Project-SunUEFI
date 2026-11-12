#!/usr/bin/env python3
"""Build Mesa as a native Debian package with the host's APT Python modules."""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop.package_build import run_cached


def main(argv):
    output = Path(argv[0] if argv else 'build/mesa')
    source = ROOT / 'upstream/piano-mesa-current'
    env = os.environ.copy()
    # The isolated virtualenv is for firmware font preparation, not APT modules.
    for name in ('VIRTUAL_ENV', 'PYTHONHOME', 'PYTHONPATH'):
        env.pop(name, None)
    env['PATH'] = '/usr/sbin:/usr/bin:/sbin:/bin'
    inputs = [ROOT / 'build.sh', ROOT / 'config/build-container.json',
              source / 'scripts/build-mesa-debs.sh', *sorted((source / 'patches').glob('*.patch'))]
    run_cached(output.resolve(), [path.resolve() for path in inputs],
               ['bash', source / 'scripts/build-mesa-debs.sh', output, '26.1.6-1~bpo13+1'], env)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
