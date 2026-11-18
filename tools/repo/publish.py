#!/usr/bin/env python3
"""Build signed APT/pacman indexes and publish packages to R2 without pruning."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
REPOSITORIES = ('apt', 'sunuefi', 'sunuefi-steamos')
APT_INDEXES = ('main/binary-arm64/Packages', 'main/binary-arm64/Packages.gz')
APT_INDEX_HASHES = ('SHA512', 'SHA256')
KERNEL_META_VERSION = re.compile(
    r'([0-9]+\.[0-9]+\.[0-9]+(?:~?rc[0-9]+)?)'
    r'\+([0-9]+)\.[0-9]+\.([0-9a-f]{12}(?:\.[0-9a-f]{12})?)-1')


def sha(path, algorithm='sha256'):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def apt_index_hashes(release, algorithm):
    """Read the Packages entries in one Release checksum section."""
    section = re.search(rf'^{algorithm}:\n((?:[ \t]+[^\n]+(?:\n|$))+)', release.read_text(), re.M)
    if not section:
        raise ValueError(f'APT Release has no {algorithm} section')
    width = hashlib.new(algorithm.lower()).digest_size * 2
    hashes = {}
    for line in section[1].splitlines():
        row = re.fullmatch(rf'\s+([0-9a-f]{{{width}}})\s+([0-9]+)\s+(\S+)', line)
        if not row or row[3] in hashes:
            raise ValueError(f'APT Release has a malformed {algorithm} section')
        hashes[row[3]] = (int(row[2]), row[1])
    if any(name not in hashes for name in APT_INDEXES):
        raise ValueError(f'APT Release is missing an ARM64 Packages index in {algorithm}')
    return {name: hashes[name] for name in APT_INDEXES}


def run(*args, **kwargs):
    return subprocess.run([str(value) for value in args], check=True, **kwargs)


def sign(path, key, *, clear=False):
    output = path.parent / ('InRelease' if clear else path.name + '.sig')
    run('gpg', '--batch', '--yes', '--local-user', key,
        '--output', output, '--clearsign' if clear else '--detach-sign', path)
    return output


def verify(path, signature, keyring):
    run('gpgv', '--keyring', keyring, signature, path)


def stanzas(data):
    return [row + '\n' for row in data.strip().split('\n\n') if row.strip()]


def field(stanza, name):
    match = re.search(r'^' + re.escape(name) + r': (.+)$', stanza, re.M)
    if not match:
        raise ValueError('Missing package metadata: ' + name)
    return match[1]


def check_kernel_epoch(name, version, previous):
    """Do not let an ABI digest set the upgrade order within one generation."""
    built = KERNEL_META_VERSION.fullmatch(version)
    if not built:
        return
    for indexed_version in previous:
        indexed = KERNEL_META_VERSION.fullmatch(indexed_version)
        if indexed and indexed.group(1, 2) == built.group(1, 2) and indexed[3] != built[3]:
            raise ValueError(f'{name} uses ABI epoch {built[2]} for both {indexed_version} '
                             f'and {version}. Advance the kernel abi_epoch and package_epoch '
                             'in config/release.json before publishing a different ABI.')


class Store:
    def __init__(self):
        import boto3
        from botocore.config import Config
        self.bucket = os.environ['R2_BUCKET']
        self.client = boto3.client(
            's3', endpoint_url=os.environ['R2_ENDPOINT'], region_name='auto',
            aws_access_key_id=os.environ['R2_ACCESS_KEY_ID'],
            aws_secret_access_key=os.environ['R2_SECRET_ACCESS_KEY'],
            config=Config(signature_version='s3v4',
                          request_checksum_calculation='when_required',
                          response_checksum_validation='when_required',
                          retries={'mode': 'standard', 'max_attempts': 5}))

    def get(self, key, path):
        from botocore.exceptions import ClientError
        try:
            response = self.client.get_object(Bucket=self.bucket, Key=key)
        except ClientError as error:
            if error.response['Error']['Code'] in ('NoSuchKey', '404'):
                return False
            raise
        with response['Body'] as source, path.open('wb') as target:
            shutil.copyfileobj(source, target)
        return True

    def put(self, key, path, *, immutable=False):
        from botocore.exceptions import ClientError
        digest = sha(path)

        def already_published():
            try:
                previous = self.client.head_object(Bucket=self.bucket, Key=key)
            except ClientError as error:
                if error.response['Error']['Code'] not in ('NoSuchKey', '404', 'NotFound'):
                    raise
                return False
            if (previous.get('Metadata', {}).get('sha256') != digest or
                    previous.get('ContentLength') != path.stat().st_size):
                raise ValueError('Published filename has different bytes: ' + key)
            return True

        if immutable and already_published():
            return
        condition = {'IfNoneMatch': '*'} if immutable else {}
        with path.open('rb') as source:
            try:
                self.client.put_object(
                    Bucket=self.bucket, Key=key, Body=source,
                    ContentLength=path.stat().st_size, Metadata={'sha256': digest},
                    CacheControl=('public, max-age=31536000, immutable' if immutable else
                                  'no-cache, max-age=0, must-revalidate'), **condition)
            except ClientError as error:
                # A retry can race the original upload after its response was lost.
                if (not immutable or error.response['Error']['Code'] not in (
                        'PreconditionFailed', '412') or not already_published()):
                    raise


def copy_package(package, destination, digests):
    shutil.copyfile(package, destination)
    if digests is not None and sha(destination) != digests[package.name]:
        raise ValueError('Package changed while preparing publication: ' + package.name)


def apt_index(work, packages, key, store=None, *, digests=None):
    """Retain old package entries; publish by-hash indexes before InRelease."""
    prefix = 'apt'
    keyring = ROOT / 'config/keys/sunuefi-archive-keyring.gpg'
    pool = work / 'pool/main'
    pool.mkdir(parents=True)
    old = []
    if store:
        signed_release, release = work / 'old-InRelease', work / 'old-Release'
        if store.get('apt/dists/sid/InRelease', signed_release):
            run('gpgv', '--keyring', keyring, '--output', release, signed_release)
            index = work / 'old-Packages'
            # Previous generations may only have the SHA256 by-hash objects.
            size, digest = apt_index_hashes(release, 'SHA256')[APT_INDEXES[0]]
            if not store.get('apt/dists/sid/main/binary-arm64/by-hash/SHA256/' + digest, index):
                raise ValueError('Published APT by-hash index is missing')
            if sha(index) != digest or index.stat().st_size != size:
                raise ValueError('Published APT Packages digest differs')
            old = stanzas(index.read_text())
    for package in packages:
        architecture = run('dpkg-deb', '-f', package, 'Architecture',
                           capture_output=True, text=True).stdout.strip()
        if architecture not in ('arm64', 'all'):
            raise ValueError('APT package must be arm64 or all: ' + package.name)
        copy_package(package, pool / package.name, digests)
    data = run('dpkg-scanpackages', '--multiversion', 'pool', '/dev/null', cwd=work,
               capture_output=True, text=True).stdout
    new = stanzas(data)
    if digests is not None:
        name = 'linux-image-piano'
        previous = [field(row, 'Version') for row in old if field(row, 'Package') == name]
        for row in new:
            if field(row, 'Package') == name:
                check_kernel_epoch(name, field(row, 'Version'), previous)
    entries = {}
    for row in old + new:
        identity = tuple(field(row, name) for name in ('Package', 'Version', 'Architecture'))
        if identity in entries and field(entries[identity], 'SHA256') != field(row, 'SHA256'):
            raise ValueError('Package version was rebuilt with different bytes: ' + str(identity))
        entries[identity] = row
    target = work / 'dists/sid'
    binary = target / 'main/binary-arm64'
    binary.mkdir(parents=True)
    (binary / 'Packages').write_text('\n'.join(entries[key] for key in sorted(entries)) + '\n')
    (binary / 'Packages.gz').write_bytes(gzip.compress((binary / 'Packages').read_bytes(), mtime=0))
    # Generate Release before adding by-hash files, avoiding recursive listings.
    release = target / 'Release'
    release.write_bytes(run(
        'apt-ftparchive', '-o', 'APT::FTPArchive::Release::Origin=SunUEFI',
        '-o', 'APT::FTPArchive::Release::Suite=sid',
        '-o', 'APT::FTPArchive::Release::Codename=sid',
        '-o', 'APT::FTPArchive::Release::Architectures=arm64',
        '-o', 'APT::FTPArchive::Release::Components=main',
        '-o', 'APT::FTPArchive::Release::Acquire-By-Hash=yes',
        'release', '.', cwd=target, capture_output=True).stdout)
    immutable = list(pool.iterdir())
    for algorithm in APT_INDEX_HASHES:
        for name, (size, digest) in apt_index_hashes(release, algorithm).items():
            index = target / name
            if index.stat().st_size != size or sha(index, algorithm.lower()) != digest:
                raise ValueError(f'APT Release digest differs for {name} ({algorithm})')
            hashed = index.parent / 'by-hash' / algorithm / digest
            hashed.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(index, hashed)
            immutable.append(hashed)
    detached = sign(release, key).rename(target / 'Release.gpg')
    inrelease = sign(release, key, clear=True)
    if store:
        run('gpgv', '--keyring', keyring, inrelease)
        verify(release, detached, keyring)
    # InRelease is the APT commit point. Release and detached signature remain
    # for older clients; separate objects cannot be switched atomically.
    mutable = [binary / 'Packages', binary / 'Packages.gz', release, detached, inrelease]
    return prefix, immutable, mutable


def pacman_entries(database):
    # The builder's Python tarfile cannot read this zstd archive directly.
    data = run('bsdtar', '-cf', '-', '@' + str(database), capture_output=True).stdout
    selected = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for member in archive:
            if not member.name.endswith('/desc'):
                continue
            if not member.isfile():
                raise ValueError('Invalid pacman index entry: ' + member.name)
            record = archive.extractfile(member).read().decode()
            fields = []
            for field in ('NAME', 'VERSION', 'FILENAME', 'SHA256SUM'):
                values = re.findall(rf'^%{field}%\n(\S+)$', record, re.M)
                if len(values) != 1:
                    raise ValueError(f'{database.name}: invalid {field} in {member.name}')
                fields.append(values[0])
            name, version, filename, digest = fields
            if name in selected:
                raise ValueError(f'{database.name}: multiple entries for {name}')
            selected[name] = (version, filename, digest)
    return selected


def check_pacman_selection(database, expected):
    """Check the archives pacman will select from the generated database."""
    selected = pacman_entries(database)
    for name, (version, filename, digest) in expected.items():
        if name not in selected:
            raise ValueError(f'{database.name}: {name} {version} is missing from the index')
        indexed_version, indexed_file, indexed_digest = selected[name]
        if indexed_version != version:
            raise ValueError(f'{database.name}: {name} {version} is required for this build, '
                             f'but the index selects {indexed_version}. '
                             'Use a build with a package version newer than the indexed version.')
        if (indexed_file, indexed_digest) != (filename, digest):
            raise ValueError(f'{database.name}: {name} {version} does not match the sealed '
                             f'filename and SHA-256 for {filename}')


def pacman_index(work, packages, name, key, store=None, *, digests=None):
    prefix = 'pacman/' + name + '/os/aarch64'
    db = work / (name + '.db.tar.zst')
    files = work / (name + '.files.tar.zst')
    archives = [(path, path.with_name(path.name + '.sig')) for path in (db, files)]
    sources = [path for pair in archives for path in pair]
    committed = work / 'index.json'
    keyring = ROOT / 'config/keys/sunuefi-archive-keyring.gpg'
    if store and store.get(prefix + '/' + committed.name, committed):
        index = json.loads(committed.read_text())
        if not isinstance(index, dict) or set(index) != {path.name for path in sources}:
            raise ValueError('Published pacman index has unexpected files')
        for path in sources:
            digest = index[path.name]
            if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError('Published pacman index has an invalid digest: ' + path.name)
            if not store.get(prefix + '/by-hash/SHA256/' + digest, path) or sha(path) != digest:
                raise ValueError('Published pacman index is missing or damaged: ' + path.name)
        for archive, signature in archives:
            verify(archive, signature, keyring)
    immutable, expected = [], {}
    for package in packages:
        target = work / package.name
        copy_package(package, target, digests)
        info = run('bsdtar', '-xOf', target, '.PKGINFO', capture_output=True, text=True).stdout
        if not re.search(r'^arch = (aarch64|any)$', info, re.M):
            raise ValueError('pacman package must be aarch64 or any: ' + package.name)
        if digests is not None:
            names = re.findall(r'^pkgname = (\S+)$', info, re.M)
            versions = re.findall(r'^pkgver = (\S+)$', info, re.M)
            if len(names) != 1 or len(versions) != 1:
                raise ValueError('Missing or repeated package name or version: ' + package.name)
            if names[0] in expected:
                raise ValueError(f'{name}: multiple packages for {names[0]} in the build')
            expected[names[0]] = (versions[0], package.name, digests[package.name])
        signature = target.with_name(target.name + '.sig')
        # Reuse the signature for an unchanged package; GPG signatures carry time.
        if not store or not store.get(prefix + '/' + signature.name, signature):
            sign(target, key)
        verify(target, signature, keyring)
        immutable.extend((target, signature))
    if digests is not None and db.is_file() and 'linux-piano' in expected:
        indexed = pacman_entries(db).get('linux-piano')
        if indexed:
            check_kernel_epoch('linux-piano', expected['linux-piano'][0], [indexed[0]])
    run('repo-add', '--prevent-downgrade', '--include-sigs', '--sign', '--key', key, db,
        *[work / package.name for package in packages])
    if store:
        for archive, signature in archives:
            verify(archive, signature, keyring)
    if digests is not None:
        check_pacman_selection(db, expected)
    mutable = []
    index = {}
    for source in sources:
        alias = work / source.name.replace('.tar.zst', '')
        if alias.is_symlink():
            alias.unlink()
        shutil.copyfile(source, alias)
        mutable.extend((source, alias))
        digest = sha(source)
        hashed = work / 'by-hash/SHA256' / digest
        hashed.parent.mkdir(parents=True, exist_ok=True)
        if not hashed.exists():
            shutil.copyfile(source, hashed)
            immutable.append(hashed)
        index[source.name] = digest
    # libalpm reads the standard aliases. The next publisher reads only a
    # complete generation, even if an upload stopped between the aliases.
    committed.write_text(json.dumps(index, sort_keys=True) + '\n')
    return prefix, immutable, [*mutable, committed]


def prepare_publication(args, store=None, *, digests=None):
    """Build and verify local indexes before making any repository writes."""
    if args.repo not in REPOSITORIES:
        raise ValueError('Unknown package repository: ' + args.repo)
    packages = [path.resolve() for path in args.packages]
    names = {path.name for path in packages}
    if not packages or len(names) != len(packages):
        raise ValueError('Pass a nonempty package list with unique filenames')
    if digests is not None and set(digests) != names:
        raise ValueError('Package checksums do not match the files selected for publication')
    for path in packages:
        pattern = r'[A-Za-z0-9_.+~:%-]+\.deb' if args.repo == 'apt' else (
            r'[A-Za-z0-9_.+~:%-]+\.pkg\.tar\.(zst|xz|gz)')
        if not path.is_file() or not re.fullmatch(pattern, path.name):
            raise ValueError('Invalid package filename: ' + path.name)
    if args.output.exists():
        raise ValueError('Use a fresh output directory')
    args.output.mkdir(parents=True)
    work = args.output.resolve()
    if args.repo == 'apt':
        prefix, immutable, mutable = apt_index(work, packages, args.key, store, digests=digests)
    else:
        prefix, immutable, mutable = pacman_index(
            work, packages, args.repo, args.key, store, digests=digests)
    if digests is None:
        digests = {path.name: sha(path) for path in packages}
    return SimpleNamespace(repo=args.repo, output=work, prefix=prefix, immutable=immutable,
                           mutable=mutable, packages=digests)


def upload_publication(publication, store=None):
    if store:
        for name in ('sunuefi-archive-keyring.gpg', 'sunuefi-archive-keyring.asc'):
            store.put('keys/' + name, ROOT / 'config/keys' / name)
        prefix = publication.prefix + '/'
        for path in publication.immutable:
            store.put(prefix + path.relative_to(publication.output).as_posix(), path, immutable=True)
        for path in publication.mutable:
            store.put(prefix + path.relative_to(publication.output).as_posix(), path)
    return {'repo': publication.repo, 'published': store is not None,
            'packages': publication.packages}


def publish(args, store=None):
    return upload_publication(prepare_publication(args, store), store)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', choices=REPOSITORIES, required=True)
    parser.add_argument('--key', required=True, help='GPG signing key fingerprint')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--publish', action='store_true', help='Merge and upload the R2 repository')
    parser.add_argument('packages', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        print(json.dumps(publish(args, Store() if args.publish else None), indent=2))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
