"""Check the newly published indexes at the URLs package managers use."""
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time
from urllib.parse import urlsplit

from publish import (APT_INDEX_HASHES, APT_INDEXES, REPOSITORIES, ROOT, apt_index_hashes,
                     field, pacman_metadata, run, sha, stanzas)


HTTP_TIMEOUT = 15
RETRY_DELAYS = (2, 4, 8, 16, 32)
INDEX_SIZE_LIMIT = 16 * 1024 * 1024


class PublicIndexError(ValueError):
    """A published index is unavailable or does not match the new generation."""


def fetch(public_url, key, destination, limit):
    try:
        result = run('curl', '--disable', '--silent', '--show-error', '--fail',
                     '--max-time', HTTP_TIMEOUT, '--max-filesize', limit,
                     '--output', destination, '--write-out', '%{http_code}',
                     public_url + '/' + key, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        raise PublicIndexError(f'{key}: {error.stderr.strip()}') from error
    if result.stdout != '200':
        raise PublicIndexError(f'{key}: HTTP {result.stdout}')


def download(public_url, key, destination, size, digest, *, algorithm='sha256'):
    fetch(public_url, key, destination, size)
    if (not destination.is_file() or destination.stat().st_size != size
            or sha(destination, algorithm) != digest):
        raise PublicIndexError(f'{key}: bytes do not match the published generation')


def check_signature(keyring, key, *args):
    try:
        run('gpgv', '--keyring', keyring, *args, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip().splitlines()
        reason = detail[-1] if detail else 'gpgv exited with an error'
        raise PublicIndexError(f'{key}: signature verification failed: {reason}') from error


def checked_public_url(public_url):
    address = urlsplit(public_url)
    if (address.scheme not in ('https', 'http') or not address.hostname
            or address.username is not None or '?' in public_url or '#' in public_url):
        raise ValueError('Use a public HTTP(S) base URL without credentials, query or fragment')
    return public_url.rstrip('/')


def archive_keyring():
    keyring = ROOT / 'config/keys/sunuefi-archive-keyring.gpg'
    if not keyring.is_file():
        raise ValueError('Project archive keyring is missing: ' + str(keyring))
    return keyring


def published_kernel_versions(public_url, repository, package):
    """Read versions only from a verified index; an unavailable index is not proof."""
    if repository not in REPOSITORIES:
        raise ValueError('Unknown package repository: ' + repository)
    public_url = checked_public_url(public_url)
    keyring = archive_keyring()
    try:
        with tempfile.TemporaryDirectory(prefix='sunuefi-kernel-index-') as temporary:
            work = Path(temporary)
            if repository == 'apt':
                prefix = 'apt/dists/sid'
                key = prefix + '/InRelease'
                signed, release = work / 'InRelease', work / 'Release'
                fetch(public_url, key, signed, INDEX_SIZE_LIMIT)
                check_signature(keyring, key, '--output', release, signed)
                index = work / 'Packages'
                size, digest = apt_index_hashes(release, 'SHA256')[APT_INDEXES[0]]
                download(public_url, prefix + '/main/binary-arm64/by-hash/SHA256/' + digest,
                         index, size, digest)
                return [field(row, 'Version') for row in stanzas(index.read_text())
                        if field(row, 'Package') == package]

            key = f'pacman/{repository}/os/aarch64/{repository}.db'
            database = work / (repository + '.db')
            signature = work / (repository + '.db.sig')
            fetch(public_url, key, database, INDEX_SIZE_LIMIT)
            fetch(public_url, key + '.sig', signature, INDEX_SIZE_LIMIT)
            check_signature(keyring, key, signature, database)
            selected, _ = pacman_metadata(database)
            return [selected[package][0]] if package in selected else []
    except PublicIndexError:
        raise
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        raise PublicIndexError(f'{repository}: {error}') from error


def verify_public(public_url, repository, output):
    """Raise unless the client-facing signed indexes match this publication."""
    if repository not in REPOSITORIES:
        raise ValueError('Unknown package repository: ' + repository)
    public_url = checked_public_url(public_url)
    keyring = archive_keyring()

    if repository == 'apt':
        prefix = 'apt/dists/sid'
        names = ('InRelease',)
        folder = Path(output) / 'dists/sid'
    else:
        prefix = f'pacman/{repository}/os/aarch64'
        names = tuple(repository + ext for ext in ('.db', '.db.sig', '.files', '.files.sig'))
        folder = Path(output)
    expected = {name: ((folder / name).stat().st_size, sha(folder / name)) for name in names}

    # R2's public cache can still return a valid older index or a cached 404
    # after the write is visible through S3. Recheck the same client URLs.
    for delay in (*RETRY_DELAYS, None):
        try:
            with tempfile.TemporaryDirectory(prefix='sunuefi-public-index-') as temporary:
                work = Path(temporary)
                for name, (size, digest) in expected.items():
                    download(public_url, prefix + '/' + name, work / name, size, digest)
                if repository == 'apt':
                    release = work / 'Release'
                    check_signature(keyring, prefix + '/InRelease',
                                    '--output', release, work / 'InRelease')
                    for algorithm in APT_INDEX_HASHES:
                        for name, (size, digest) in apt_index_hashes(release, algorithm).items():
                            directory = name.rsplit('/', 1)[0]
                            key = f'{prefix}/{directory}/by-hash/{algorithm}/{digest}'
                            download(public_url, key, work / digest, size, digest,
                                     algorithm=algorithm.lower())
                else:
                    for suffix in ('.db', '.files'):
                        name = repository + suffix
                        check_signature(keyring, prefix + '/' + name,
                                        work / (name + '.sig'), work / name)
            return
        except PublicIndexError as error:
            if delay is None:
                raise PublicIndexError(
                    f'{repository}: public index check failed after {len(RETRY_DELAYS) + 1} attempts: '
                    f'{error}. Check the public domain and cache rules.') from error
            time.sleep(delay)
