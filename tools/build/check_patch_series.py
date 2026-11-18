#!/usr/bin/env python3
"""Check patch order, shared files and supported product targets offline."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from ci_build_targets import TARGETS, changed_files


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch_id(path, *, verbatim=False):
    """Include plain unified diffs, which git patch-id otherwise skips."""
    lines = path.read_text().splitlines(keepends=True)
    unified, git_header = [], False
    for index, line in enumerate(lines):
        if line.startswith('diff --git '):
            git_header = True
        if (line.startswith('--- ') and index + 1 < len(lines) and
                lines[index + 1].startswith('+++ ')):
            if not git_header:
                before = line[4:].split('\t', 1)[0].strip()
                after = lines[index + 1][4:].split('\t', 1)[0].strip()
                name = before if after == '/dev/null' else after
                name = re.sub(r'^[ab]/', '', name)
                unified.append(f'diff --git a/{name} b/{name}\n')
            git_header = False
        unified.append(line)
    mode = '--verbatim' if verbatim else '--stable'
    result = subprocess.run(['git', 'patch-id', mode], input=''.join(unified),
                            text=True, capture_output=True, check=True)
    return tuple(line.split()[0] for line in result.stdout.splitlines())


def family(root, series):
    relative = series.relative_to(root)
    if relative.parts[0] == 'patches':
        return root.joinpath(*relative.parts[:2])
    return root / 'linux/bsp/patches' / relative.parts[3]


def upstream_errors(metadata, label):
    if not isinstance(metadata, dict):
        return [label + ': upstream must be an object']
    errors = []
    if metadata.get('status') not in ('merged', 'submitted', 'downstream'):
        errors.append(label + ': upstream status must be merged, submitted or downstream')
    if not isinstance(metadata.get('url'), str) or not metadata['url'].startswith('https://'):
        errors.append(label + ': upstream needs its HTTPS source or submission URL')
    if not isinstance(metadata.get('drop_when'), str) or not metadata['drop_when'].strip():
        errors.append(label + ': upstream needs a concrete drop_when condition')
    commit = metadata.get('commit')
    if commit is not None and not re.fullmatch('[0-9a-f]{40}', str(commit)):
        errors.append(label + ': upstream commit must be a complete Git object ID')
    return errors


def metadata_coverage(manifests, references):
    coverage = defaultdict(list)
    for patch, rows in references.items():
        for series, row in rows:
            if 'upstream' in row:
                coverage[patch].append(row['upstream'])
            elif 'upstream' in manifests[series]:
                coverage[patch].append(manifests[series]['upstream'])
    return coverage


def previous_matrix_families(root, base):
    """Keep registered families strict even if every targets field is removed."""
    if not base:
        return set()
    if not re.fullmatch('[0-9a-f]{40}', base):
        raise ValueError('Patch series base must be a complete Git commit ID')
    if base == '0' * 40:
        return set()
    exists = subprocess.run(['git', '-C', root, 'cat-file', '-e', base + '^{commit}'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if exists.returncode:
        # changed_files selects all tracked inputs for a missing push base.
        return set()
    names = subprocess.check_output(
        ['git', '-C', root, 'ls-tree', '-r', '--name-only', base,
         'patches', 'linux/bsp/patches'], text=True).splitlines()
    result = set()
    for name in names:
        path = root / name
        if not path.name.startswith('series') or path.suffix != '.json':
            continue
        data = json.loads(subprocess.check_output(
            ['git', '-C', root, 'show', base + ':' + name], text=True))
        if isinstance(data, dict) and 'targets' in data:
            result.add(family(root, path))
    return result


def check(root=ROOT, products=TARGETS, changed=(), require_all=False, previous_families=()):
    root = Path(root).resolve()
    products = set(products)
    errors, manifests, references = [], {}, defaultdict(list)
    declared_families = set(previous_families)
    all_families = set()
    for directory in (root / 'patches', root / 'linux/bsp/patches'):
        if not directory.exists():
            continue
        for path in sorted(directory.rglob('series*.json')):
            label = path.relative_to(root).as_posix()
            try:
                data = json.loads(path.read_text())
                if not isinstance(data, dict):
                    raise ValueError('series must be an object')
                lists = [key for key in ('patches', 'series') if key in data]
                if len(lists) != 1 or not isinstance(data[lists[0]], list):
                    raise ValueError('use one ordered patches or series list')
                manifests[path] = data
                all_families.add(family(root, path))
                targets = data.get('targets')
                if targets is not None:
                    declared_families.add(family(root, path))
                    if (not isinstance(targets, list) or not targets or
                            any(not isinstance(target, str) for target in targets)):
                        errors.append(label + ': targets must be a nonempty product list')
                    elif len(set(targets)) != len(targets) or set(targets) - products:
                        errors.append(label + ': targets must name distinct CI products: ' +
                                      ', '.join(sorted(products)))
                if 'upstream' in data:
                    errors.extend(upstream_errors(data['upstream'], label))
                seen = set()
                for index, row in enumerate(data[lists[0]]):
                    entry_label = f'{label} entry {index + 1}'
                    if not isinstance(row, dict):
                        errors.append(entry_label + ': patch entry must be an object')
                        continue
                    fields = [key for key in ('file', 'path', 'patch') if key in row]
                    if len(fields) != 1 or not isinstance(row[fields[0]], str):
                        errors.append(entry_label + ': use one file, path or patch')
                        continue
                    name = row[fields[0]]
                    patch = (path.parent / name).resolve()
                    if (Path(name).is_absolute() or not patch.is_relative_to(root) or
                            patch.suffix != '.patch' or not patch.is_file()):
                        errors.append(entry_label + ': missing or unsafe patch: ' + name)
                        continue
                    if patch in seen:
                        errors.append(entry_label + ': patch occurs twice in this series: ' + name)
                    seen.add(patch)
                    if row.get('sha256') != digest(patch):
                        errors.append(entry_label + ': patch SHA256 differs: ' + name)
                    references[patch].append((path, row))
                    if 'upstream' in row:
                        errors.extend(upstream_errors(row['upstream'], entry_label))
            except (OSError, ValueError) as error:
                errors.append(label + ': ' + str(error))

    changed_patches = set()
    for name in changed:
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            continue
        path = root / relative
        if not path.is_file():
            continue
        if path in manifests:
            declared_families.add(family(root, path))
        elif path.suffix == '.patch' and path.is_relative_to(root / 'patches'):
            changed_patches.add(path.resolve())
        elif path.suffix == '.patch' and path.is_relative_to(root / 'linux/bsp/patches'):
            changed_patches.add(path.resolve())
    for patch in changed_patches:
        if patch not in references:
            errors.append(patch.relative_to(root).as_posix() + ': changed patch is outside every series')
        for series, _ in references[patch]:
            declared_families.add(family(root, series))
    if require_all:
        declared_families.update(all_families)

    # A shared file can be stored in another version or package. Require its
    # provenance once across all references, not once per importing series.
    for path, data in manifests.items():
        if family(root, path) in declared_families and 'targets' not in data:
            errors.append(path.relative_to(root).as_posix() + ': version has no CI products')
    for directory in sorted(declared_families):
        for patch in sorted(directory.rglob('*.patch')):
            if patch.resolve() not in references:
                errors.append(patch.relative_to(root).as_posix() + ': patch is outside every series')
    if require_all:
        for directory in (root / 'patches', root / 'linux/bsp/patches'):
            for patch in sorted(directory.rglob('*.patch')):
                if (patch.resolve() not in references and
                        not any(patch.is_relative_to(folder) for folder in declared_families)):
                    errors.append(patch.relative_to(root).as_posix() + ': patch is outside every series')

    coverage = metadata_coverage(manifests, references)
    missing_upstream, hashes, changes = [], defaultdict(list), defaultdict(list)
    for path in sorted(references):
        hashes[digest(path)].append(path)
        try:
            identity = patch_id(path)
            if identity:
                changes[identity].append(path)
            else:
                errors.append(path.relative_to(root).as_posix() + ': no unified or Git diff found')
        except (OSError, subprocess.SubprocessError, UnicodeError) as error:
            errors.append(path.relative_to(root).as_posix() + ': cannot identify patch: ' + str(error))
        metadata = coverage[path]
        if not metadata:
            name = path.relative_to(root).as_posix()
            missing_upstream.append(name)
            if any(family(root, series) in declared_families for series, _ in references[path]):
                errors.append(name + ': retained patch needs upstream status, URL and drop_when')
        if metadata and any(item != metadata[0] for item in metadata[1:]):
            errors.append(path.relative_to(root).as_posix() + ': shared upstream metadata conflicts')
    duplicates = set()
    for groups, reason in ((hashes, 'identical files'), (changes, 'same Git patch-id')):
        for paths in groups.values():
            key = tuple(paths)
            if len(paths) < 2 or key in duplicates:
                continue
            duplicates.add(key)
            errors.append(reason + '; share one patch file: ' + ', '.join(
                path.relative_to(root).as_posix() for path in paths))
    return {'series': len(manifests), 'patches': len(references),
            'shared_files': sum(len(rows) > 1 for rows in references.values()),
            'matrix_series': sum('targets' in data for data in manifests.values()),
            'unregistered_series': [path.relative_to(root).as_posix()
                                    for path, data in manifests.items() if 'targets' not in data],
            'missing_upstream': missing_upstream,
            'errors': errors}


def check_revision(root=ROOT, base=None, require_all=False):
    root = Path(root).resolve()
    changed = changed_files(root, base) if base else ()
    previous = previous_matrix_families(root, base)
    return check(root, changed=changed, require_all=require_all, previous_families=previous)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--changed-since', help='Require metadata for series and patches changed since this commit')
    parser.add_argument('--require-all', action='store_true', help='Reject all unregistered series and missing patch provenance')
    args = parser.parse_args()
    report = check_revision(args.root, args.changed_since, args.require_all)
    for error in report['errors']:
        print(error, file=sys.stderr)
    if report['errors']:
        return 1
    print(f"Patch series: {report['series']} series, {report['patches']} files, "
          f"{report['shared_files']} shared, {report['matrix_series']} with CI products")
    print(f"Metadata gaps: {len(report['unregistered_series'])} unregistered series, "
          f"{len(report['missing_upstream'])} patches without upstream status and drop_when")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
