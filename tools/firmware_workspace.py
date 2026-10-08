#!/usr/bin/env python3
"""Prepare disposable firmware sources without editing the upstream checkouts."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
MARKER = '.firmware-workspace.json'
WORKSPACE = 'build/firmware-workspace'
COPY_ROOTS = ('tools', 'uefi', 'config', 'vendor', 'patches')
FIRMWARE_REPOS = ('upstream/Mu-Silicium', 'upstream/simple-init')


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], stderr=subprocess.PIPE)


def _record(workspace):
    record = json.loads((workspace / MARKER).read_text())
    origin = Path(record['original_root']).resolve()
    if record.get('schema_version') != 1 or workspace.resolve() != origin / WORKSPACE:
        raise ValueError('Invalid firmware workspace source registration')
    return record, origin


def active_root(root):
    """Select the prepared workspace; ordinary fixture roots remain unchanged."""
    root = Path(root).resolve()
    if (root / MARKER).is_file():
        _record(root)
        return root
    workspace = root / WORKSPACE
    if (workspace / MARKER).is_file():
        _, origin = _record(workspace)
        if origin != root:
            raise ValueError('Firmware workspace belongs to a different source root')
        return workspace
    return root


def _tracked_sources(root):
    paths = {p.decode() for p in git(root, 'ls-files', '--cached', '--others',
                                   '--exclude-standard', '-z', '--', *COPY_ROOTS).split(b'\0') if p}
    paths.add('sources.lock.json')
    # This helper is needed while its first implementation is still untracked.
    paths.add('tools/firmware_workspace.py')
    return sorted(p for p in paths if not p.startswith('uefi/platforms/pianoProductPkg/'))


def _check_repo(path, commit):
    top = Path(git(path, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
    head = git(path, 'rev-parse', 'HEAD').decode().strip()
    if top != path.resolve() or head != commit:
        raise ValueError('Unexpected firmware source commit: ' + str(path))


def _repositories(root):
    lock = json.loads((root / 'sources.lock.json').read_text())
    repos = {}

    def visit(relative, commit):
        source = root / relative
        _check_repo(source, commit)
        repos[relative] = commit
        for entry in git(source, 'ls-tree', '-rz', '--full-tree', commit).split(b'\0'):
            if not entry:
                continue
            metadata, name = entry.split(b'\t', 1)
            mode, _, nested_commit = metadata.decode().split()
            if mode != '160000':
                continue
            child = relative + '/' + name.decode()
            locked = lock.get(child.removeprefix('upstream/'))
            if not (root / child / '.git').exists():
                if locked is not None:
                    raise ValueError('Required firmware dependency is missing; run ./build.sh sources: ' + child)
                continue  # Upstream's optional test/fuzz repositories stay uninitialized.
            pinned = locked['commit'] if locked is not None else nested_commit
            if pinned != nested_commit:
                raise ValueError('Firmware lock differs from the parent gitlink: ' + child)
            visit(child, pinned)

    for relative in FIRMWARE_REPOS:
        visit(relative, lock[relative.removeprefix('upstream/')]['commit'])
    return repos


def _link(path, target):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        if path.resolve() == target.resolve():
            return
        path.unlink()
    elif path.exists():
        raise ValueError('Expected a firmware workspace link: ' + str(path))
    path.symlink_to(target, target_is_directory=True)


def prepare(root=ROOT):
    root = Path(root).resolve()
    if (root / MARKER).is_file():
        _, root = _record(root)
    workspace = root / WORKSPACE
    previous = {}
    if workspace.exists():
        if workspace.is_symlink() or not (workspace / MARKER).is_file():
            raise ValueError('Refusing to replace an unregistered firmware workspace')
        previous, origin = _record(workspace)
        if origin != root:
            raise ValueError('Firmware workspace belongs to a different source root')
    paths = _tracked_sources(root)
    repos = _repositories(root)
    for relative in paths:
        if not (root / relative).is_file():
            raise ValueError('Tracked firmware source is missing: ' + relative)
    workspace.mkdir(parents=True, exist_ok=True)
    record = {'schema_version': 1, 'original_root': str(root),
              'copied_paths': paths, 'repositories': repos}
    # Register the disposable directory before cloning so interrupted local
    # preparation can be repeated without touching any original checkout.
    (workspace / MARKER).write_text(json.dumps(record, indent=2) + '\n')
    for relative in previous.get('copied_paths', []):
        if relative not in paths:
            (workspace / relative).unlink(missing_ok=True)
    for relative in paths:
        destination = workspace / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            destination.unlink()
        shutil.copy2(root / relative, destination)
    for relative, commit in repos.items():
        destination = workspace / relative
        if not (destination / '.git').exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['git', 'clone', '--local', '--no-checkout', '--',
                            str(root / relative), str(destination)], check=True, capture_output=True)
        if subprocess.run(['git', '-C', str(destination), 'cat-file', '-e',
                           commit + '^{commit}'], capture_output=True).returncode:
            git(destination, 'fetch', '--no-tags', str(root / relative), commit)
        # Only disposable workspace checkouts are reset. Reapplying a changed
        # patch/overlay must start from its recorded upstream commit.
        git(destination, 'checkout', '--detach', '--force', commit)
        git(destination, 'clean', '-ffdx', '-e', 'Build/', '-e', 'build/', '-e', 'Conf/')
        _check_repo(destination, commit)
    for relative in ('artifacts', 'build/logs'):
        (root / relative).mkdir(parents=True, exist_ok=True)
        _link(workspace / relative, root / relative)
    for relative in ('.venv', 'build/host-tools'):
        _link(workspace / relative, root / relative)
    # Product display preparation only reads these fixed reference sources.
    for relative in ('upstream/linux-piano', 'upstream/dtc'):
        if (root / relative).is_dir():
            _link(workspace / relative, root / relative)
    return workspace


def validate_sources(root, relative_paths=None):
    """Reject stale canonical copies used by a build, from either root."""
    workspace = active_root(root)
    if not (workspace / MARKER).is_file():
        return True
    record, origin = _record(workspace)
    copied = set(record['copied_paths'])
    selected = copied if relative_paths is None else copied.intersection(str(p) for p in relative_paths)
    for relative in sorted(selected):
        if Path(relative).suffix.lower() in ('.md', '.rst'):
            continue
        source, projected = origin / relative, workspace / relative
        if not source.is_file() or not projected.is_file() or source.read_bytes() != projected.read_bytes():
            raise ValueError('Firmware workspace source is stale; rebuild: ' + relative)
    for relative, commit in record['repositories'].items():
        _check_repo(origin / relative, commit)
        _check_repo(workspace / relative, commit)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'build', 'validate'), nargs='?', default='prepare')
    args = parser.parse_args()
    try:
        if args.operation in ('prepare', 'build'):
            workspace = prepare()
            if args.operation == 'build':
                os.execv('/bin/bash', ['bash', str(workspace / 'tools/build_product.sh')])
            print(workspace)
        else:
            validate_sources(ROOT)
            print(active_root(ROOT))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))


if __name__ == '__main__':
    main()
