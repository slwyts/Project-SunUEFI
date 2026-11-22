"""Keep the archives and signed index objects used by recent publications."""
from collections import defaultdict
from functools import cache
import json
from pathlib import Path
import re
import subprocess
import tempfile

from publish import (APT_INDEX_HASHES, DEBIAN_ARCHIVE, PACMAN_ARCHIVE, REPOSITORIES,
                     apt_index_hashes, field, pacman_metadata, pacman_index_paths, stanzas)


KEEP = 5
HISTORY = 'package-history.json'
APT_POOL = 'apt/pool/main/'
APT_SID = 'apt/dists/sid/'
PACKAGE_NAME = r'[a-z0-9][a-z0-9.+-]*'
VERSIONED_KERNEL = re.compile(
    r'linux-(?:image-|headers-|piano-(?:dtb-|v4l2loopback-)?)'
    r'([0-9][a-z0-9._-]*-b[1-9][0-9]*-g[0-9a-f]{12})(?:-headers)?')
DEBIAN_DEPENDS = re.compile(r'^(?:Pre-)?Depends: (.+)$', re.M)
DEBIAN_RELATION = re.compile(rf'({PACKAGE_NAME})(?::[a-z0-9-]+)?(?:\s*\(=\s*(\S+)\))?')


def kernel_release(name):
    match = VERSIONED_KERNEL.fullmatch(name)
    return match[1] if match else None


def debian_filename(row):
    path = field(row, 'Filename')
    if not path.startswith('pool/main/') or not DEBIAN_ARCHIVE.fullmatch(
            filename := path.removeprefix('pool/main/')):
        raise ValueError('Unexpected APT archive path: ' + path)
    return filename


@cache
def newer_debian(version, previous):
    result = subprocess.run(['dpkg', '--compare-versions', version, 'gt', previous])
    if result.returncode not in (0, 1):
        raise ValueError(f'Cannot compare Debian versions {version} and {previous}')
    return result.returncode == 0


def current_apt_entries(entries, recent):
    """Keep current package versions, recently published files and their dependencies."""
    latest, exact, families = {}, defaultdict(list), defaultdict(list)
    selected = set()
    for identity, row in entries.items():
        name, version, arch = identity
        package = (name, arch)
        if package not in latest or newer_debian(version, latest[package][1]):
            latest[package] = identity
        exact[name, version].append(identity)
        if debian_filename(row) in recent:
            selected.add(identity)
    for (name, _), identity in latest.items():
        if release := kernel_release(name):
            families[release].append(identity)
        else:
            selected.add(identity)

    queue = list(selected)

    def keep(identities):
        for identity in identities:
            if identity not in selected:
                selected.add(identity)
                queue.append(identity)

    for identity in queue:
        if release := kernel_release(identity[0]):
            keep(families.pop(release, ()))
        for line in DEBIAN_DEPENDS.findall(entries[identity]):
            for relation in re.split(r'\s*[,|]\s*', line):
                if dependency := DEBIAN_RELATION.match(relation):
                    name, version = dependency.groups()
                    if version:
                        keep(exact[name, version])
                    elif kernel_release(name):
                        keep(latest[key] for key in ((name, 'arm64'), (name, 'all'))
                             if key in latest)
    return {identity: entries[identity] for identity in selected}


class History:
    def __init__(self, repository, store, publication_id, started_at, archives):
        if repository not in REPOSITORIES:
            raise ValueError('Unknown package repository: ' + repository)
        if not isinstance(publication_id, str) or not re.fullmatch(r'[1-9][0-9]*', publication_id):
            raise ValueError('Package publication requires a GitHub workflow run ID')
        self.repo = repository
        self.prefix = ('apt/' if repository == 'apt' else
                       f'pacman/{repository}/os/aarch64/')
        self.store = store
        self.id = publication_id
        self.started_at = started_at
        self.archives = set(archives)
        self.previous_index_objects = set()
        self.releases = self.load()
        previous = [record for record in self.releases if record['run'] != self.id]
        self.recent_releases = previous[-(KEEP - 1):]
        self.expired_archives = self.package_objects(
            name for record in previous[:-(KEEP - 1)] for name in record['packages'])
        self.previous_attempt = next((record for record in self.releases
                                      if record['run'] == self.id), None)
        self.recent = self.archives | {name for record in self.recent_releases
                                       for name in record['packages']}
        if self.previous_attempt:
            self.recent.update(self.previous_attempt['packages'])

    def is_archive(self, key):
        if self.repo == 'apt':
            return key.startswith(APT_POOL) and DEBIAN_ARCHIVE.fullmatch(key.removeprefix(APT_POOL))
        return key.startswith(self.prefix) and PACMAN_ARCHIVE.fullmatch(
            key.removeprefix(self.prefix).removesuffix('.sig'))

    def is_index(self, key):
        if self.repo == 'apt':
            return re.fullmatch(re.escape(APT_SID) + r'main/binary-arm64/by-hash/'
                                r'(?:SHA256/[0-9a-f]{64}|SHA512/[0-9a-f]{128})', key)
        return re.fullmatch(re.escape(self.prefix) + r'by-hash/SHA256/[0-9a-f]{64}', key)

    def manages(self, key):
        return self.is_archive(key) or self.is_index(key)

    def valid_snapshot(self, objects):
        return (isinstance(objects, list) and
                all(isinstance(key, str) and self.manages(key) for key in objects) and
                any(self.is_index(key) for key in objects))

    def load(self):
        with tempfile.TemporaryDirectory(prefix='sunuefi-package-history-') as temporary:
            path = Path(temporary) / HISTORY
            if not self.store.get(self.prefix + HISTORY, path):
                return []
            history = json.loads(path.read_text())
        if (not isinstance(history, dict) or history.get('schema_version') != 1 or
                history.get('repo') != self.repo or
                not isinstance(history.get('publications'), list) or
                not 0 < len(history['publications']) <= KEEP):
            raise ValueError(f'Invalid package publication history for {self.repo}')
        runs = set()
        pattern = DEBIAN_ARCHIVE if self.repo == 'apt' else PACMAN_ARCHIVE
        for record in history['publications']:
            if (not isinstance(record, dict) or
                    not isinstance(record.get('run'), str) or
                    not re.fullmatch(r'[1-9][0-9]*', record['run']) or record['run'] in runs or
                    type(record.get('started_at')) is not int or record['started_at'] <= 0 or
                    not isinstance(record.get('packages'), list) or not record['packages'] or
                    not all(isinstance(name, str) and pattern.fullmatch(name)
                            for name in record['packages']) or
                    not isinstance(record.get('snapshots'), list) or
                    not all(self.valid_snapshot(snapshot) for snapshot in record['snapshots'])):
                raise ValueError(f'Invalid package publication record for {self.repo}')
            runs.add(record['run'])
        return history['publications']

    def package_objects(self, archives):
        if self.repo == 'apt':
            return {APT_POOL + name for name in archives}
        return {self.prefix + name + suffix for name in archives for suffix in ('', '.sig')}

    def apt_objects(self, rows, release):
        return self.package_objects(debian_filename(row) for row in rows) | {
            f'{APT_SID}{name.rsplit("/", 1)[0]}/by-hash/{algorithm}/{digest}'
            for algorithm in APT_INDEX_HASHES
            for name, (_, digest) in apt_index_hashes(release, algorithm).items()
        }

    def pacman_objects(self, work):
        indexes = json.loads((work / 'index.json').read_text())
        archives = {filename for database, _ in pacman_index_paths(work, self.repo)
                    for _, filename, _ in pacman_metadata(database)[0].values()}
        keys = self.package_objects(archives) | {
            self.prefix + 'by-hash/SHA256/' + digest for digest in indexes.values()
        }
        if not all(self.manages(key) for key in keys):
            raise ValueError(f'Unexpected package object in the {self.repo} index')
        return keys

    def observe_apt(self, rows, release):
        if self.releases:
            self.previous_index_objects = self.apt_objects(rows, release)

    def observe_pacman(self, work):
        if self.releases:
            self.previous_index_objects = self.pacman_objects(work)

    def apt_entries(self, entries):
        return current_apt_entries(entries, self.recent)

    def pacman_retired_kernels(self, selected, records):
        anchors = {name for name, (_, archive, _) in selected.items() if archive in self.recent}
        for meta in ('linux-piano', 'linux-piano-headers'):
            record = records.get(meta, '')
            field = re.search(r'^%DEPENDS%\n(.+?)(?:\n\n|\Z)', record, re.M | re.S)
            if field:
                anchors.update(re.split('[<>=]', name, 1)[0] for name in field[1].splitlines())
        releases = {kernel_release(name) for name in anchors}
        return {name for name in selected
                if (release := kernel_release(name)) and release not in releases}

    def record(self, work):
        if self.repo == 'apt':
            dist = work / 'dists/sid'
            rows = stanzas((dist / 'main/binary-arm64/Packages').read_text())
            current_objects = self.apt_objects(rows, dist / 'Release')
        else:
            current_objects = self.pacman_objects(work)
        keep_archives = self.package_objects(self.recent) | {
            key for key in current_objects if self.is_archive(key)
        }
        current = sorted(current_objects)
        snapshots = [current]
        previous = sorted(self.previous_index_objects)
        if previous and previous != current and not any(
                previous in record['snapshots'] for record in self.releases):
            snapshots.append(previous)
        archives, started_at = set(self.archives), self.started_at
        if self.previous_attempt:
            # A retry can be rebuilt with new package revisions. The prior
            # successful attempt is still part of the same workflow run.
            snapshots.extend(snapshot for snapshot in self.previous_attempt['snapshots']
                             if snapshot not in snapshots)
            archives.update(self.previous_attempt['packages'])
            started_at = min(started_at, self.previous_attempt['started_at'])
        if not all(self.valid_snapshot(snapshot) for snapshot in snapshots):
            raise ValueError(f'Unexpected object in the {self.repo} publication')
        self.releases = [*self.recent_releases, {'run': self.id, 'started_at': started_at,
                                               'packages': sorted(archives), 'snapshots': snapshots}]
        self.obsolete = self.plan_prune(current_objects, keep_archives)
        path = work / HISTORY
        path.write_text(json.dumps({'schema_version': 1, 'repo': self.repo,
                                    'publications': self.releases}, sort_keys=True) + '\n')
        self.store.put(self.prefix + HISTORY, path)

    def plan_prune(self, current_objects, keep_archives):
        # Earlier publications have no history entry. Give their clients a full
        # retention window rather than guessing which old archives they use.
        if len(self.releases) < KEEP:
            return ()
        cutoff = min(record['started_at'] for record in self.releases)
        prefixes = (APT_POOL, APT_SID + 'main/binary-arm64/by-hash/') if self.repo == 'apt' else (
            self.prefix,)
        existing = dict(item for prefix in prefixes for item in self.store.list_objects(prefix))
        if missing := current_objects - existing.keys():
            raise ValueError(f'{self.repo}: cannot clean up; published object is missing: {min(missing)}')
        for record in self.releases:
            record['snapshots'] = [
                snapshot for snapshot in record['snapshots']
                if set(snapshot) <= existing.keys() and
                all(key in keep_archives or self.is_index(key) for key in snapshot)
            ]
        keep_indexes = {
            key for record in self.releases for snapshot in record['snapshots']
            for key in snapshot if self.is_index(key)
        }
        obsolete_indexes = {key for key in existing if self.is_index(key) and key not in keep_indexes}
        obsolete_archives = {key for key, modified in existing.items()
                             if self.is_archive(key) and key not in keep_archives and
                             (key in self.expired_archives or modified < cutoff)}
        return obsolete_indexes, obsolete_archives

    def prune(self):
        # Withdraw indexes before deleting any package they might still name.
        for obsolete in self.obsolete:
            if obsolete:
                self.store.delete(obsolete)
        return sum(map(len, self.obsolete))
