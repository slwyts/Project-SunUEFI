#!/usr/bin/env python3
"""Get or check the fixed build sources. Does not reset edits or touch a device."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE_GROUPS = {
    'firmware': ('Mu-Silicium', 'simple-init', 'dtc'),
    'kernel': ('v4l2loopback',),
    'devices': ('debian-piano-current', 'piano-sensors-current', 'audioreach-topology'),
}


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def expected(root, group='all'):
    lock = json.loads((root / 'sources.lock.json').read_text())
    if group != 'all':
        selected = SOURCE_GROUPS[group]
        missing = set(selected) - lock.keys()
        if missing:
            raise ValueError(f'Missing {group} source pins: ' + ', '.join(sorted(missing)))
        lock = {name: value for name, value in lock.items() if name.split('/', 1)[0] in selected}
    return {'upstream/' + name: value['commit'] for name, value in lock.items()}


def check(root, group='all'):
    rows = expected(root, group)
    failures = []
    for name, commit in rows.items():
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
    return len(rows)


def submodule_batches(rows):
    """Only initialize paths listed in the lock, with parents before children."""
    repositories = {Path(name) for name in rows}
    batches = {}
    for repository in sorted(repositories):
        parent = next((path for path in repository.parents if path in repositories), Path('.'))
        batches.setdefault(parent, []).append(repository.relative_to(parent))
    return sorted(batches.items(), key=lambda batch: len(batch[0].parts))


def prepare(root, group='all'):
    # No --force or --remote: Git rejects changes that cannot be preserved and
    # checks out the gitlink, not a moving branch tip.
    for parent, children in submodule_batches(expected(root, group)):
        subprocess.run(['git', '-C', str(root / parent), 'submodule', 'update',
                        '--init', '--depth', '1', '--', *children], check=True)
    return check(root, group)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', choices=('all', *SOURCE_GROUPS), default='all',
                        help='Fetch only the fixed inputs for one build stage')
    parser.add_argument('--check', action='store_true', help='Verify only; no network or checkout changes')
    args = parser.parse_args()
    try:
        count = check(ROOT, args.group) if args.check else prepare(ROOT, args.group)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
    print(f'{count} fixed source checkouts verified. Applied overlays are checked by the product builder.')


if __name__ == '__main__':
    main()
