#!/usr/bin/env python3
"""Prepare the release kernel from a public baseline and tracked mail patches.

Host-only. No kernel build, device access, branch switch, or original-target
fetch is performed. Failed worktrees are preserved for inspection.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    'kernel69': {
        'base_commit': '352508459733d3e6d349ea5581a8dd2fd8bb4180',
        'target_tree': '17d484d4771a140e1eeb18eaf0315028adc5a16a',
        'original_target_commit': 'efe5734c24510c3c194f765511b49be9f13b4aa0',
        'upstream_base_commit': '500df175a7f9e6bc1a9c328590ca5150f84f9ff0',
        'source_url': 'https://github.com/blu-sharky/linux-piano.git',
        'patch_commits': [
            '1fd91b90a7204c7846f2fc0580cc967a20d692cf',
            '74c517825a95113f35c121db136e3880d44b9806',
            'd42158782b81c4aaa47c8643f1400a471785370b',
            '40bee45195195997dca6603821e27cc233057d4d',
            '0b04714515a7236aebd0847b9b3806493321151d',
            'e23f4479f6bd35eec3f1438e8c022e6a27d8fc8f',
            'd4c8b9fadef5167b68b8aa7160255f62c0c4cb82',
            'efe5734c24510c3c194f765511b49be9f13b4aa0',
        ],
    },
}
COMMITTER_NAME = 'SunUEFI source preparation'
COMMITTER_EMAIL = 'source-preparation@example.invalid'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(repository, *args, env=None, check=True):
    result = subprocess.run(
        ['git', '-C', str(repository), *map(str, args)], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if check and result.returncode:
        raise ValueError('Git command failed: ' + result.stderr.strip()[:6000])
    return result.stdout.strip() if check else result


def object_id(value, label):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Invalid ' + label)
    return value


def load_series(root, target):
    if target not in TARGETS:
        raise ValueError('Unknown release kernel target: ' + str(target))
    policy = TARGETS[target]
    folder = root / 'patches/linux'
    manifest_path = folder / 'series.json'
    raw = manifest_path.read_bytes()
    series = json.loads(raw)
    if series.get('schema_version') != 1 or series.get('target') != target:
        raise ValueError('Patch series schema/target mismatch')
    for key in ('base_commit', 'target_tree', 'original_target_commit', 'source_url'):
        if series.get(key) != policy[key]:
            raise ValueError('Patch series pin mismatch: ' + key)
    object_id(series.get('base_tree'), 'base tree')
    rows = series.get('patches')
    if not isinstance(rows, list) or not 1 <= len(rows) <= 256:
        raise ValueError('Invalid patch series length')
    if policy.get('patch_commits') is not None and [row.get('original_commit') for row in rows] != policy['patch_commits']:
        raise ValueError('Original patch commit sequence changed')
    previous = policy['base_commit']
    seen = set()
    patches = []
    for row in rows:
        name = row.get('file', '')
        if not re.fullmatch(r'[0-9]{4}-[A-Za-z0-9._-]+\.patch', name) or name in seen:
            raise ValueError('Invalid/duplicate patch filename')
        seen.add(name)
        commit = object_id(row.get('original_commit'), 'original commit')
        if row.get('original_parent') != previous:
            raise ValueError('Original patch parent chain changed')
        previous = commit
        object_id(row.get('tree'), 'patch tree')
        date = row.get('committer_date')
        if not isinstance(date, str):
            raise ValueError('Invalid original committer date')
        try:
            parsed_date = datetime.datetime.fromisoformat(date)
        except ValueError as exc:
            raise ValueError('Invalid original committer date') from exc
        if parsed_date.tzinfo is None:
            raise ValueError('Original committer date needs a timezone')
        path = folder / name
        if path.is_symlink() or path.resolve().parent != folder.resolve():
            raise ValueError('Patch path escapes its series directory')
        data = path.read_bytes()
        if sha(data) != row.get('sha256'):
            raise ValueError('Patch SHA256 mismatch: ' + name)
        if not data.startswith(('From ' + commit + ' Mon Sep 17 00:00:00 2001\n').encode()):
            raise ValueError('Patch original commit header mismatch: ' + name)
        patches.append((path, dict(row)))
    if previous != policy['original_target_commit'] or rows[-1]['tree'] != policy['target_tree']:
        raise ValueError('Patch series target mismatch')
    return series, patches, sha(raw)


def is_repository(path):
    if not path.is_dir():
        return False
    bare = git(path, 'rev-parse', '--is-bare-repository', check=False)
    if bare.returncode:
        return False
    if bare.stdout.strip() == 'true':
        location = git(path, 'rev-parse', '--absolute-git-dir')
    else:
        location = git(path, 'rev-parse', '--show-toplevel')
    # An empty, uninitialized submodule directory would otherwise resolve to
    # the enclosing SunUEFI repository, polluting it with the kernel fetch.
    return Path(location).resolve() == path.resolve()


def ensure_repository(root, repository, policy):
    if repository is not None:
        repo = Path(repository).resolve()
        if not is_repository(repo):
            raise ValueError('Expected an explicit kernel repository root')
    else:
        repo = root / 'upstream/linux-piano'
        if not is_repository(repo):
            repo = root / 'build/kernel-sources/linux-piano.git'
            if not repo.exists():
                repo.parent.mkdir(parents=True, exist_ok=True)
                result = subprocess.run(['git', 'init', '--bare', str(repo)], capture_output=True, text=True)
                if result.returncode:
                    raise ValueError('Cannot create release source repository: ' + result.stderr.strip())
            if not is_repository(repo):
                raise ValueError('Invalid project-local kernel source repository')
    required = [policy['base_commit']]
    if policy.get('upstream_base_commit'):
        required.append(policy['upstream_base_commit'])
    for commit in required:
        if git(repo, 'cat-file', '-e', commit + '^{commit}', check=False).returncode:
            # The URL is fixed in TARGETS; user remotes and original local
            # target commit are never used as fallback fetch sources.
            args = ['fetch', '--no-tags']
            if git(repo, 'rev-parse', '--is-shallow-repository') == 'true':
                args.append('--unshallow')
            git(repo, *args, policy['source_url'], commit)
        if git(repo, 'rev-parse', commit + '^{commit}') != commit:
            raise ValueError('Public baseline object mismatch')
    return repo


def clean(work):
    if git(work, 'status', '--porcelain=v1', '--untracked-files=all'):
        raise ValueError('Release kernel worktree is dirty')
    flags = git(work, 'ls-files', '-v').splitlines()
    if any(line and (line[0].islower() or line[0] == 'S') for line in flags):
        raise ValueError('Release kernel index hides working-tree changes')
    git_dir = Path(git(work, 'rev-parse', '--absolute-git-dir'))
    if (git_dir / 'rebase-apply').exists():
        raise ValueError('Release kernel patch application is incomplete')


def write_manifest(path, record):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    if temporary.exists():
        raise ValueError('Preserve pending source manifest: ' + str(temporary))
    temporary.write_text(json.dumps(record, indent=2) + '\n')
    temporary.replace(path)


def prepare(root=ROOT, repository=None, worktree=None, source_manifest=None, target='kernel69'):
    root = Path(root).resolve()
    series, patches, series_hash = load_series(root, target)
    policy = TARGETS[target]
    work = Path(worktree or root / 'build/kernel-worktrees/release-kernel').resolve()
    marker = Path(source_manifest or root / 'build/release-kernel/source-manifest.json').resolve()
    if not work.is_relative_to(root / 'build/kernel-worktrees'):
        raise ValueError('Release worktree must stay under project build/kernel-worktrees')
    if not marker.is_relative_to(root / 'build') or marker.is_relative_to(work):
        raise ValueError('Source manifest must stay under project build and outside the worktree')
    if work.exists():
        if not marker.is_file():
            raise ValueError('Existing release worktree has no completed source manifest; preserve it')
        record = json.loads(marker.read_text())
        if (record.get('status') != 'SOURCE_PREPARED_NOT_BUILT' or record.get('target') != target or
                record.get('series_sha256') != series_hash or record.get('worktree') != str(work) or
                record.get('public_baseline') != policy['base_commit'] or
                record.get('original_target_commit') != policy['original_target_commit'] or
                record.get('actual_tree') != policy['target_tree']):
            raise ValueError('Existing release source manifest mismatch; preserve the worktree')
        clean(work)
        if git(work, 'rev-parse', 'HEAD') != record.get('actual_commit') or git(work, 'rev-parse', 'HEAD^{tree}') != policy['target_tree']:
            raise ValueError('Prepared release source HEAD/tree drifted')
        if git(work, 'merge-base', '--is-ancestor', policy['base_commit'], record['actual_commit'], check=False).returncode:
            raise ValueError('Prepared source no longer descends from the public baseline')
        return record
    if marker.exists():
        raise ValueError('Source manifest exists without its worktree; preserve it')
    repo = ensure_repository(root, repository, policy)
    if git(repo, 'rev-parse', policy['base_commit'] + '^{tree}') != series['base_tree']:
        raise ValueError('Public baseline tree mismatch')
    record = {
        'schema_version': 1, 'status': 'SOURCE_PREPARATION_IN_PROGRESS', 'target': target,
        'source_url': policy['source_url'], 'public_baseline': policy['base_commit'],
        'base_tree': series['base_tree'], 'original_target_commit': policy['original_target_commit'],
        'expected_tree': policy['target_tree'], 'series_sha256': series_hash,
        'repository': str(repo), 'worktree': str(work), 'patches': [],
        'tool_sha256': sha(Path(__file__).read_bytes()), 'kernel_built': False,
        'device_operation_performed': False,
    }
    current_patch = None
    try:
        work.parent.mkdir(parents=True, exist_ok=True)
        git(repo, '-c', 'core.hooksPath=/dev/null', 'worktree', 'add', '--detach', work, policy['base_commit'])
        for path, row in patches:
            current_patch = path.name
            env = os.environ.copy()
            env.update({'GIT_COMMITTER_NAME': COMMITTER_NAME, 'GIT_COMMITTER_EMAIL': COMMITTER_EMAIL,
                        'GIT_COMMITTER_DATE': row['committer_date']})
            git(work, '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null',
                '-c', 'am.threeWay=false', 'am', '--whitespace=error', path, env=env)
            applied_commit = git(work, 'rev-parse', 'HEAD')
            applied_tree = git(work, 'rev-parse', 'HEAD^{tree}')
            record.update(actual_commit=applied_commit, actual_tree=applied_tree)
            record['patches'].append({**row, 'applied_commit': applied_commit, 'applied_tree': applied_tree})
            if applied_tree != row['tree']:
                raise ValueError('Applied patch tree mismatch: ' + path.name)
        if git(work, 'rev-parse', 'HEAD^{tree}') != policy['target_tree']:
            raise ValueError('Final release kernel tree mismatch')
        clean(work)
        if load_series(root, target)[2] != series_hash:
            raise ValueError('Patch series changed during preparation')
        record['status'] = 'SOURCE_PREPARED_NOT_BUILT'
        write_manifest(marker, record)
        return record
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        record.update(status='SOURCE_PREPARATION_FAILED', failing_patch=current_patch, error=str(exc))
        if work.exists() and not git(work, 'rev-parse', 'HEAD', check=False).returncode:
            record['actual_commit'] = git(work, 'rev-parse', 'HEAD')
            record['actual_tree'] = git(work, 'rev-parse', 'HEAD^{tree}')
        write_manifest(marker, record)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, help='Existing kernel Git repository; default is project-local')
    parser.add_argument('--worktree', type=Path, help='Fresh worktree under project build/kernel-worktrees')
    parser.add_argument('--source-manifest', type=Path, help='New provenance record under project build')
    parser.add_argument('--target', default='kernel69')
    args = parser.parse_args()
    try:
        result = prepare(repository=args.repository, worktree=args.worktree,
                         source_manifest=args.source_manifest, target=args.target)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, str(exc) + '\n')
    print(json.dumps({key: result[key] for key in ('status', 'worktree', 'actual_commit', 'actual_tree', 'public_baseline')}, indent=2))


if __name__ == '__main__':
    main()
