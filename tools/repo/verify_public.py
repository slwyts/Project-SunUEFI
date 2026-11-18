"""Check the newly published indexes at the URLs package managers use."""
from pathlib import Path
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

from publish import APT_INDEX_HASHES, REPOSITORIES, ROOT, apt_index_hashes, run, sha


HTTP_TIMEOUT = 15
RETRY_DELAYS = (2, 4, 8, 16, 32)


class PublicIndexError(ValueError):
    """A published index is unavailable or does not match the new generation."""


def download(public_url, key, destination, size, digest, *, algorithm='sha256'):
    try:
        result = run('curl', '--disable', '--silent', '--show-error', '--fail',
                     '--max-time', HTTP_TIMEOUT, '--max-filesize', size,
                     '--output', destination, '--write-out', '%{http_code}',
                     public_url + '/' + key, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        raise PublicIndexError(f'{key}: {error.stderr.strip()}') from error
    if result.stdout != '200':
        raise PublicIndexError(f'{key}: HTTP {result.stdout}')
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


def verify_public(public_url, repository, output):
    """Raise unless the client-facing signed indexes match this publication."""
    if repository not in REPOSITORIES:
        raise ValueError('Unknown package repository: ' + repository)
    address = urlsplit(public_url)
    if (address.scheme not in ('https', 'http') or not address.hostname
            or address.username is not None or '?' in public_url or '#' in public_url):
        raise ValueError('Use a public HTTP(S) base URL without credentials, query or fragment')
    public_url = public_url.rstrip('/')
    keyring = ROOT / 'config/keys/sunuefi-archive-keyring.gpg'
    if not keyring.is_file():
        raise ValueError('Project archive keyring is missing: ' + str(keyring))

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
