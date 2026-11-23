#!/usr/bin/env python3
"""Copy a successful product build's system images to the release downloads."""
from contextlib import suppress
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from zipfile import ZipFile

from botocore.exceptions import ClientError

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from ci_build_targets import DISTROS
from package.boot import PUBLIC_REPOSITORY_URL
from repo.publish import Store, sha


PART_BYTES = 64 * 1024 * 1024
SHA256 = r'[0-9a-f]{64}'
RELEASE_CACHE = 'public, max-age=31536000, immutable'
# The public domain can still serve a cached 404 after R2 accepts the upload.
# Allow the default three-minute lifetime for a cached 404 to elapse.
RANGE_RETRY_DELAYS = (2, 5, 10, 30, 60, 120)
ROOT_ARTIFACTS = {target: f'SunUEFI-Root-{target}' for target in DISTROS}
REQUIRED_ARTIFACTS = {
    *ROOT_ARTIFACTS.values(), 'logs', 'SunUEFI-ESP', 'SunUEFI-Module', 'SunUEFI-UEFI-Fastboot',
}


def gh_json(path, *options):
    return json.loads(subprocess.check_output(['gh', 'api', path, *options]))


def product_run(repository, repository_id, run_id, source_sha):
    endpoint = f'/repos/{repository}/actions/runs/{run_id}'
    run = gh_json(endpoint)
    if (run['path'] != '.github/workflows/build-products.yml'
            or run['head_branch'] != 'main' or run['head_sha'] != source_sha
            or (run['head_repository'] or {}).get('id') != repository_id):
        raise ValueError('The Build products run does not match the source commit on main.')
    return run


def recoverable_targets(repository, run):
    """Accept a completed build whose only failure was in download publication."""
    if (run['status'], run['conclusion']) != ('completed', 'failure'):
        raise ValueError('Select a completed build with a failed download job.')
    endpoint = f"/repos/{repository}/actions/runs/{run['id']}/attempts/{run['run_attempt']}/jobs"
    pages = gh_json(endpoint + '?per_page=100', '--paginate', '--slurp')
    jobs = {job['name']: job['conclusion'] for page in pages for job in page['jobs']}
    failed = {name for name, conclusion in jobs.items() if conclusion == 'failure'}
    if (not failed or not failed <= {'downloads / publish', 'downloads / prune'}
            or any(result not in ('success', 'skipped', 'failure') for result in jobs.values())
            or any(jobs.get(name) != 'success' for name in (
                'checks / check', 'shared / shared (firmware)', 'logs',
            ))):
        raise ValueError('The build failed before its download jobs; rerun the failed build jobs.')
    targets = [target for target in DISTROS if jobs.get(f'build / build ({target})') == 'success']
    if targets and jobs.get('repository / publish') != 'success':
        raise ValueError('The signed package repository was not published.')
    return targets


def download_zip(repository, artifact, path):
    with path.open('wb') as output:
        subprocess.run(
            ['gh', 'api', f"/repos/{repository}/actions/artifacts/{artifact['id']}/zip"],
            stdout=output, check=True,
        )
    if (path.stat().st_size != artifact['size_in_bytes']
            or f'sha256:{sha(path)}' != artifact['digest']):
        raise ValueError(f'The downloaded ZIP does not match GitHub: {path.name}')


def image_digests(logs):
    with ZipFile(logs) as archive:
        for target in DISTROS:
            entry = f'build-logs-{target}/artifacts/fastboot-sparse/SHA256SUMS'
            sums = archive.read(entry).decode()
            names = [('root.ext4.img', ROOT_ARTIFACTS[target], f'root-{target}.sparse.img')]
            if target == DISTROS[0]:
                names.append(('esp.img', 'SunUEFI-ESP', 'esp.sparse.img'))
            for original, artifact, filename in names:
                match = re.search(rf'^({SHA256})  {re.escape(original)}$', sums, re.M)
                if not match:
                    raise ValueError(f'The build has no {original} checksum for {target}.')
                yield artifact, (filename, match[1])


def verify_image(path, filename, digest):
    with ZipFile(path) as archive:
        if archive.namelist() != [filename]:
            raise ValueError(f'{path.name} must contain only {filename}.')
        with archive.open(filename) as image:
            if hashlib.file_digest(image, 'sha256').hexdigest() != digest:
                raise ValueError(f'{filename} does not match the build logs.')


def already_uploaded(client, location, size, metadata):
    try:
        result = client.head_object(**location)
    except ClientError as error:
        if error.response['Error']['Code'] in ('404', 'NotFound', 'NoSuchKey'):
            return False
        raise
    if (result['ContentLength'] != size
            or any(result.get('Metadata', {}).get(key) != value
                   for key, value in metadata.items())):
        raise ValueError(f"A different build already uses {location['Key']}.")
    return result


def download_headers(filename, metadata, cache_control):
    return {
        'ContentType': 'application/zip',
        'ContentDisposition': f'attachment; filename="{filename}"',
        'CacheControl': cache_control,
        'Metadata': metadata,
    }


def upload_parts(client, location, size, headers, write_parts):
    upload = client.create_multipart_upload(**location, **headers)
    reference = dict(location, UploadId=upload['UploadId'])
    try:
        client.complete_multipart_upload(
            **reference, MultipartUpload={'Parts': list(write_parts(reference))}, IfNoneMatch='*',
        )
    except Exception:
        with suppress(ClientError):
            client.abort_multipart_upload(**reference)
        if not already_uploaded(client, location, size, headers['Metadata']):
            raise


def upload_zip(client, location, path, metadata, *, cache_control=RELEASE_CACHE):
    def write_parts(reference):
        with path.open('rb') as stream:
            for number, data in enumerate(iter(lambda: stream.read(PART_BYTES), b''), 1):
                part = client.upload_part(**reference, PartNumber=number, Body=data)
                yield {'ETag': part['ETag'], 'PartNumber': number}

    upload_parts(client, location, path.stat().st_size,
                 download_headers(path.name, metadata, cache_control), write_parts)


def copy_zip(client, source, target, size, metadata):
    if not already_uploaded(client, source, size, metadata):
        raise ValueError(f"The build download is missing: {source['Key']}.")
    headers = download_headers(Path(target['Key']).name, metadata, RELEASE_CACHE)

    def write_parts(reference):
        # R2 enforces the destination condition on multipart completion.
        # Small source objects must be copied without a byte range.
        for number, start in enumerate(range(0, size, PART_BYTES), 1):
            last = min(start + PART_BYTES, size) - 1
            extent = {'CopySourceRange': f'bytes={start}-{last}'} if size > PART_BYTES else {}
            part = client.upload_part_copy(
                **reference, CopySource=source, PartNumber=number, **extent,
            )
            yield {'ETag': part['CopyPartResult']['ETag'], 'PartNumber': number}

    upload_parts(client, target, size, headers, write_parts)


def verify_range(curl, url, size):
    byte = size - 1
    expected = ['206', f'bytes {byte}-{byte}/{size}', '1']
    probe = [*curl, '--range', f'{byte}-{byte}', '--max-time', '30', '--max-filesize', '1',
             '--output', '/dev/null',
             '--write-out', '%{response_code}\n%header{content-range}\n%{size_download}\n', url]
    for delay in (*RANGE_RETRY_DELAYS, None):
        result = subprocess.run(probe, capture_output=True, text=True)
        response = result.stdout.splitlines()
        if not result.returncode and response == expected:
            return
        detail = (f'expected {expected!r}, received {response!r}, curl exited {result.returncode}'
                  + (f': {result.stderr.strip()}' if result.stderr.strip() else ''))
        if delay is None:
            raise ValueError(f'Download does not support resuming: {url} ({detail})')
        print(f'Range check for {url}: {detail}. Retrying in {delay}s.', flush=True)
        time.sleep(delay)


def verify_download(url, size, digest=None, *, archive=None):
    curl = ['curl', '--disable', '--fail', '--silent', '--show-error', '--location',
            '--header', 'Accept-Encoding: identity']
    verify_range(curl, url, size)
    if digest is not None:
        download = [*curl, '--speed-limit', '1024', '--speed-time', '60']
        if archive is not None:
            subprocess.run([*download, '--output', archive, url], check=True)
            status, fetched = 0, sha(archive)
        else:
            with subprocess.Popen([*download, url], stdout=subprocess.PIPE) as transfer:
                fetched = hashlib.file_digest(transfer.stdout, 'sha256').hexdigest()
                status = transfer.wait()
        if status or fetched != digest:
            raise ValueError(f'Public download does not match the build: {url}')


def build_manifest(store, run, checkout_sha, path):
    # Rerunning only cleanup does not create a new published download list.
    for attempt in range(run['run_attempt'], 0, -1):
        prefix = f"builds/{run['id']}/{attempt}"
        if store.get(f'{prefix}/downloads.json', path):
            break
    else:
        raise ValueError('This build does not have a download list in Cloudflare R2.')
    record = json.loads(path.read_text())
    if (record.get('schema_version') != 1 or record.get('storage') != 'Cloudflare R2'
            or record.get('source_sha') != checkout_sha or record.get('run_id') != run['id']
            or record.get('run_attempt') != attempt
            or record['files'].keys() != REQUIRED_ARTIFACTS):
        raise ValueError('The R2 download list must describe this complete build.')
    for name, row in record['files'].items():
        filename = f'{name}.zip'
        url = f'{PUBLIC_REPOSITORY_URL}/{prefix}/{filename}'
        if (row['file'] != filename or row['url'] != url
                or type(row['size']) is not int or row['size'] <= 0
                or not re.fullmatch(SHA256, row['sha256'])):
            raise ValueError(f'Invalid build download: {name}.')
    return prefix, attempt, record['files']


def publish_files(store, run, source_sha, tag, directory):
    archive_dir = directory / 'source'
    archive_dir.mkdir()
    build_prefix, attempt, files = build_manifest(
        store, run, source_sha, archive_dir / 'downloads.json',
    )

    prefix = f'releases/{tag}'
    downloads = {}
    for name, row in files.items():
        location = {'Bucket': store.bucket, 'Key': f"{prefix}/{row['file']}"}
        metadata = {'sha256': row['sha256'], 'source-commit': source_sha}
        present = already_uploaded(store.client, location, row['size'], metadata)
        downloads[name] = location, metadata, present

    logs = archive_dir / 'logs.zip'
    if (not store.get(f'{build_prefix}/{logs.name}', logs)
            or logs.stat().st_size != files['logs']['size']
            or sha(logs) != files['logs']['sha256']):
        raise ValueError('The R2 build logs do not match their download entry.')
    image_sha = dict(image_digests(logs))
    logs.unlink()

    results, checksums = {}, []
    for name, row in sorted(files.items()):
        filename, size, digest = row['file'], row['size'], row['sha256']
        location, metadata, present = downloads[name]
        print(f'Checking {filename}', flush=True)
        if not present:
            source = {'Bucket': store.bucket, 'Key': f'{build_prefix}/{filename}'}
            copy_zip(store.client, source, location, size, metadata)
        url = f"{PUBLIC_REPOSITORY_URL}/{location['Key']}"
        if name in image_sha:
            path = archive_dir / filename
            verify_download(url, size, digest, archive=path)
            verify_image(path, *image_sha[name])
            path.unlink()
        else:
            verify_download(url, size, digest)
        results[name] = {'file': filename, 'url': url, 'sha256': digest, 'size': size}
        checksums.append(f'{digest}  {filename}')
        if name in image_sha:
            raw_name, raw_digest = image_sha[name]
            results[name]['sparse_image_sha256'] = raw_digest
            checksums.append(f'{raw_digest}  {raw_name}')

    checksum_file = directory / 'SHA256SUMS'
    checksum_file.write_text('\n'.join(checksums) + '\n')
    store.put(f'{prefix}/SHA256SUMS', checksum_file, immutable=True)
    url = f'{PUBLIC_REPOSITORY_URL}/{prefix}/SHA256SUMS'
    verify_download(url, checksum_file.stat().st_size, sha(checksum_file))
    return {
        'schema_version': 1, 'storage': 'Cloudflare R2', 'tag': tag,
        'source_sha': source_sha, 'run_id': run['id'], 'run_attempt': attempt,
        'checksums_url': url, 'files': results,
    }


def main():
    repository = os.environ['GITHUB_REPOSITORY']
    repository_id = int(os.environ['GITHUB_REPOSITORY_ID'])
    tag, run_id, source_sha = (
        os.environ[key] for key in ('RELEASE_TAG', 'SOURCE_RUN', 'SOURCE_SHA')
    )
    if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?', tag):
        raise ValueError('Enter a release tag such as v1.0.0.')
    if not re.fullmatch(r'[1-9][0-9]*', run_id) or not re.fullmatch(r'[0-9a-f]{40}', source_sha):
        raise ValueError('Enter a Build products run ID and its full commit SHA.')
    run = product_run(repository, repository_id, int(run_id), source_sha)
    if ((run['status'], run['conclusion']) != ('completed', 'success')
            and set(recoverable_targets(repository, run)) != set(DISTROS)):
        raise ValueError('The recovered build must include all six Linux images.')
    store = Store()
    temp = Path(os.environ['RUNNER_TEMP'])
    with TemporaryDirectory(dir=temp) as directory:
        release = publish_files(store, run, source_sha, tag, Path(directory))
    manifest = temp / 'downloads.json'
    manifest.write_text(json.dumps(release, indent=2, sort_keys=True) + '\n')
    key = f'releases/{tag}/downloads.json'
    store.put(key, manifest, immutable=True)
    verify_download(f'{PUBLIC_REPOSITORY_URL}/{key}', manifest.stat().st_size, sha(manifest))
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as summary:
        run_url = f'https://github.com/{repository}/actions/runs/{run_id}'
        summary.write(f'Published to Cloudflare R2 from [Build products]({run_url})'
                      f' at `{source_sha}`.\n\n')
        for row in release['files'].values():
            summary.write(f"- [{row['file']}]({row['url']})\n")
        summary.write(f"\n[SHA256SUMS]({release['checksums_url']})\n")


if __name__ == '__main__':
    main()
