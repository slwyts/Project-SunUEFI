"""Fetch only the inputs to the selected Denial overlay from its signed release."""
import fcntl
from pathlib import Path
import shutil
import tempfile
from urllib.parse import quote
import urllib.request

from desktop import denial_packages
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[2]


def release_url(source):
    repository = source['repository'].removesuffix('.git')
    return f"{repository}/releases/download/{quote(source['tag'], safe='')}/"


def fetch_archives(source, package_format):
    tag = source['tag']
    location = release_url(source)
    cache = ROOT / 'build/source-cache/denial' / tag
    cache.mkdir(parents=True, exist_ok=True)
    checksums = denial_packages.CHECKSUMS
    signature = checksums + '.sig'
    with (cache / 'download.lock').open('a') as lock, denial_packages.release_keyring() as keyring:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not all((cache / name).is_file() for name in (checksums, signature)):
            with tempfile.TemporaryDirectory(prefix='denial-download-', dir=cache) as staging:
                staging = Path(staging)
                for name in (checksums, signature):
                    with (urllib.request.urlopen(location + name, timeout=60) as incoming,
                          (staging / name).open('wb') as output):
                        shutil.copyfileobj(incoming, output)
                digests, _ = denial_packages.release_checksums(staging, keyring)
                for name in (checksums, signature):
                    (staging / name).replace(cache / name)
        else:
            digests, _ = denial_packages.release_checksums(cache, keyring)
        inputs = denial_packages.release_archives(digests, package_format, tag.removeprefix('v'))
        for files in inputs.values():
            for filename, digest in files.items():
                asset = denial_packages.asset_name(filename)
                download(location + quote(asset, safe=''), cache / asset, digest)
    return cache
