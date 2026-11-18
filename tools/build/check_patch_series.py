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
from ci_build_targets import TARGETS


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


def check(root=ROOT, products=TARGETS):
    root = Path(root).resolve()
    products = set(products)
    errors, manifests, references = [], {}, defaultdict(list)
    directories = (root / 'patches', root / 'linux/bsp/patches')
    for directory in directories:
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
                targets = data.get('targets')
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

    for directory in directories:
        for patch in sorted(directory.rglob('*.patch')):
            if patch.resolve() not in references:
                errors.append(patch.relative_to(root).as_posix() + ': patch is outside every series')

    # Shared patches need provenance once across all references.
    coverage = metadata_coverage(manifests, references)
    hashes, changes = defaultdict(list), defaultdict(list)
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
            errors.append(path.relative_to(root).as_posix() +
                          ': retained patch needs upstream status, URL and drop_when')
        elif any(item != metadata[0] for item in metadata[1:]):
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
            'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    report = check(args.root)
    for error in report['errors']:
        print(error, file=sys.stderr)
    if report['errors']:
        return 1
    print(f"Patch series: {report['series']} series, {report['patches']} files, "
          f"{report['shared_files']} shared files")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
