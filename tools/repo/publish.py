#!/usr/bin/env python3
"""Build signed APT/pacman indexes and publish packages to R2 without pruning."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
REPOSITORIES = ('apt', 'sunuefi', 'sunuefi-steamos')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


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


def apt_index(work, packages, key, store=None):
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
            # Select the index by the authenticated digest, never by mutable URL.
            match = re.search(r'^ ([0-9a-f]{64}) +(\d+) main/binary-arm64/Packages$',
                              release.read_text(), re.M)
            if not match or not store.get(
                    'apt/dists/sid/main/binary-arm64/by-hash/SHA256/' + match[1], index):
                raise ValueError('Published APT by-hash index is missing')
            if sha(index) != match[1] or index.stat().st_size != int(match[2]):
                raise ValueError('Published APT Packages digest differs')
            old = stanzas(index.read_text())
    for package in packages:
        architecture = run('dpkg-deb', '-f', package, 'Architecture',
                           capture_output=True, text=True).stdout.strip()
        if architecture not in ('arm64', 'all'):
            raise ValueError('APT package must be arm64 or all: ' + package.name)
        shutil.copyfile(package, pool / package.name)
    data = run('dpkg-scanpackages', '--multiversion', 'pool', '/dev/null', cwd=work,
               capture_output=True, text=True).stdout
    entries = {}
    for row in old + stanzas(data):
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
    for index in (binary / 'Packages', binary / 'Packages.gz'):
        hashed = binary / 'by-hash/SHA256' / sha(index)
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


def pacman_index(work, packages, name, key, store=None):
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
    immutable = []
    for package in packages:
        info = run('bsdtar', '-xOf', package, '.PKGINFO', capture_output=True, text=True).stdout
        if not re.search(r'^arch = (aarch64|any)$', info, re.M):
            raise ValueError('pacman package must be aarch64 or any: ' + package.name)
        target = work / package.name
        shutil.copyfile(package, target)
        signature = target.with_name(target.name + '.sig')
        # Reuse the signature for an unchanged package; GPG signatures carry time.
        if not store or not store.get(prefix + '/' + signature.name, signature):
            sign(target, key)
        verify(target, signature, keyring)
        immutable.extend((target, signature))
    run('repo-add', '--prevent-downgrade', '--include-sigs', '--sign', '--key', key, db,
        *[work / package.name for package in packages])
    if store:
        for archive, signature in archives:
            verify(archive, signature, keyring)
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


def publish(args, store=None):
    if args.repo not in REPOSITORIES:
        raise ValueError('Unknown package repository: ' + args.repo)
    packages = [path.resolve() for path in args.packages]
    if not packages or len({path.name for path in packages}) != len(packages):
        raise ValueError('Pass a nonempty package list with unique filenames')
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
        prefix, immutable, mutable = apt_index(work, packages, args.key, store)
    else:
        prefix, immutable, mutable = pacman_index(work, packages, args.repo, args.key, store)
    if store:
        for name in ('sunuefi-archive-keyring.gpg', 'sunuefi-archive-keyring.asc'):
            store.put('keys/' + name, ROOT / 'config/keys' / name)
        for path in immutable:
            store.put(prefix + '/' + path.relative_to(work).as_posix(), path, immutable=True)
        for path in mutable:
            store.put(prefix + '/' + path.relative_to(work).as_posix(), path)
    return {'repo': args.repo, 'published': store is not None,
            'packages': {path.name: sha(path) for path in packages}}


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
