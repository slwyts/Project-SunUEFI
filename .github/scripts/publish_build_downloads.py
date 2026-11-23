#!/usr/bin/env python3
"""Publish a completed Build products download set and expire older sets."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

from botocore.exceptions import ClientError
from publish_release_images import (
    DISTROS, PUBLIC_REPOSITORY_URL, SHA256, Store,
    already_uploaded, download_zip, gh_json, product_run, recoverable_targets, upload_zip,
    verify_download,
)


BUILD_PREFIX = 'builds/'
INDEX = 'downloads.json'
KEEP_BUILDS = 3
FIRMWARE_ARCHIVES = {'SunUEFI-Module', 'SunUEFI-UEFI-Fastboot'}
PRODUCT_NAME = r'SunUEFI-(?:Module|UEFI-Fastboot|ESP|Root-[a-z0-9]+(?:-[a-z0-9]+)*)'
LOG_ARCHIVE = re.compile(r'logs-([1-9][0-9]*)')
BUILD_OBJECT = re.compile(
    rf'{BUILD_PREFIX}([1-9][0-9]*)/([1-9][0-9]*)/({re.escape(INDEX)}|(?:logs|{PRODUCT_NAME})\.zip)'
)
NOT_FOUND = ('404', 'NotFound', 'NoSuchKey')


def artifact_names(targets):
    if len(set(targets)) != len(targets) or any(target not in DISTROS for target in targets):
        raise ValueError('The build did not select valid Linux distributions.')
    return FIRMWARE_ARCHIVES | {'logs'} | (
        {'SunUEFI-ESP', *(f'SunUEFI-Root-{target}' for target in targets)} if targets else set()
    )


def build_location(run_id, attempt):
    return f'{BUILD_PREFIX}{run_id}/{attempt}/'


def public_url(key):
    return f'{PUBLIC_REPOSITORY_URL}/{key}'


def artifact_origin(artifact, run_id, repository_id, source_sha):
    origin = artifact.get('workflow_run') or {}
    return (origin.get('id') == run_id and origin.get('repository_id') == repository_id
            and origin.get('head_repository_id') == repository_id
            and origin.get('head_branch') == 'main' and origin.get('head_sha') == source_sha)


def run_artifacts(repository, run_id):
    path = f'/repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100'
    pages = gh_json(path, '--paginate', '--slurp')
    return [artifact for page in pages for artifact in page['artifacts']]


def select_artifacts(artifacts, expected, log_id, run_id, repository_id, source_sha):
    selected = {}
    for artifact in artifacts:
        name = artifact['name']
        if artifact['id'] == log_id and LOG_ARCHIVE.fullmatch(name):
            name = 'logs'
        elif name == 'logs':
            continue
        if name not in expected:
            continue
        if (name in selected or artifact['expired'] or artifact['size_in_bytes'] <= 0
                or not re.fullmatch(f'sha256:{SHA256}', artifact.get('digest') or '')
                or not artifact_origin(artifact, run_id, repository_id, source_sha)):
            raise ValueError(f'Cannot verify this build’s {name} archive.')
        selected[name] = artifact
    if missing := expected - selected.keys():
        raise ValueError('The build is missing: ' + ', '.join(sorted(missing)))
    return selected


def latest_log_id(artifacts, attempt):
    # A rerun of only the failed jobs keeps the logs from the original attempt.
    logs = [(int(match[1]), artifact['id']) for artifact in artifacts
            if (match := LOG_ARCHIVE.fullmatch(artifact['name']))
            and int(match[1]) <= attempt and not artifact['expired']]
    return max(logs, default=(0, 0))[1]


def make_index(artifacts, run_id, attempt, source_sha):
    prefix = build_location(run_id, attempt)
    files = {}
    for name, artifact in sorted(artifacts.items()):
        filename = f'{name}.zip'
        files[name] = {
            'file': filename, 'url': public_url(prefix + filename),
            'sha256': artifact['digest'].removeprefix('sha256:'), 'size': artifact['size_in_bytes'],
        }
    return {
        'schema_version': 1, 'storage': 'Cloudflare R2', 'source_sha': source_sha,
        'run_id': run_id, 'run_attempt': attempt, 'files': files,
    }


def valid_index(index, run_id, attempt):
    if (not isinstance(index, dict) or index.get('schema_version') != 1
            or index.get('storage') != 'Cloudflare R2'
            or type(index.get('run_id')) is not int or type(index.get('run_attempt')) is not int
            or index.get('run_id') != run_id or index.get('run_attempt') != attempt
            or not isinstance(index.get('source_sha'), str)
            or not re.fullmatch(r'[0-9a-f]{40}', index['source_sha'])):
        return False
    files = index.get('files')
    if not isinstance(files, dict) or not (FIRMWARE_ARCHIVES | {'logs'}) <= files.keys():
        return False
    if ('SunUEFI-ESP' in files) != any(name.startswith('SunUEFI-Root-') for name in files):
        return False
    prefix = build_location(run_id, attempt)
    for name, file in files.items():
        if (not re.fullmatch(rf'logs|{PRODUCT_NAME}', name) or not isinstance(file, dict)
                or file.get('file') != f'{name}.zip'
                or file.get('url') != public_url(f'{prefix}{name}.zip')
                or not isinstance(file.get('sha256'), str) or not re.fullmatch(SHA256, file['sha256'])
                or type(file.get('size')) is not int or file['size'] <= 0):
            return False
    return True


def object_metadata(index, digest):
    return {
        'sha256': digest, 'source-commit': index['source_sha'],
        'build-run': str(index['run_id']), 'build-attempt': str(index['run_attempt']),
    }


def stage_downloads(store, index_path, index, artifacts, repository):
    prefix = build_location(index['run_id'], index['run_attempt'])
    with TemporaryDirectory(dir=index_path.parent) as directory:
        for name, file in index['files'].items():
            print(f"Checking {file['file']}", flush=True)
            location = {'Bucket': store.bucket, 'Key': prefix + file['file']}
            metadata = object_metadata(index, file['sha256'])
            if not already_uploaded(store.client, location, file['size'], metadata):
                path = Path(directory) / file['file']
                download_zip(repository, artifacts[name], path)
                upload_zip(store.client, location, path, metadata, cache_control='public, max-age=300')
                path.unlink()
            verify_download(file['url'], file['size'])
    index_path.write_text(json.dumps(index, indent=2) + '\n')


def publish_index(store, index_path, run_id, attempt, source_sha):
    data = index_path.read_bytes()
    index = json.loads(data)
    if not valid_index(index, run_id, attempt) or index['source_sha'] != source_sha:
        raise ValueError('The download list does not match this build attempt.')
    key = build_location(run_id, attempt) + INDEX
    location = {'Bucket': store.bucket, 'Key': key}
    metadata = object_metadata(index, hashlib.sha256(data).hexdigest())
    if not already_uploaded(store.client, location, len(data), metadata):
        try:
            store.client.put_object(
                **location, Body=data, ContentLength=len(data), ContentType='application/json',
                CacheControl='no-cache, must-revalidate', Metadata=metadata, IfNoneMatch='*',
            )
        except ClientError as error:
            if (error.response['Error']['Code'] not in ('412', 'PreconditionFailed')
                    or not already_uploaded(store.client, location, len(data), metadata)):
                raise
    url = public_url(key)
    verify_download(url, len(data), metadata['sha256'])
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as outputs:
        outputs.write(f'attempt={attempt}\n')
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as summary:
        summary.write(f'[Download list ({INDEX})]({url})\n')


def read_index(store, key, run_id, attempt):
    result = store.client.get_object(Bucket=store.bucket, Key=key)
    with result['Body'] as stream:
        data = stream.read()
    index = json.loads(data)
    if (not valid_index(index, run_id, attempt)
            or result.get('Metadata') != object_metadata(index, hashlib.sha256(data).hexdigest())):
        raise ValueError(f'The ordinary build download list is invalid: {key}')
    return index


def ordinary_objects(store):
    groups = defaultdict(dict)
    pages = store.client.get_paginator('list_objects_v2').paginate(Bucket=store.bucket, Prefix=BUILD_PREFIX)
    for page in pages:
        for item in page.get('Contents', []):
            if match := BUILD_OBJECT.fullmatch(item['Key']):
                run_id, attempt, filename = match.groups()
                groups[int(run_id), int(attempt)][filename] = item['LastModified']
    return groups


def owned_build_object(store, key, run_id, attempt, *, expected=None):
    try:
        result = store.client.head_object(Bucket=store.bucket, Key=key)
    except ClientError as error:
        if error.response['Error']['Code'] in NOT_FOUND:
            return False
        raise
    metadata = result.get('Metadata') or {}
    if expected is not None:
        return metadata == expected
    return (metadata.get('build-run') == str(run_id) and metadata.get('build-attempt') == str(attempt)
            and re.fullmatch(SHA256, metadata.get('sha256', ''))
            and re.fullmatch(r'[0-9a-f]{40}', metadata.get('source-commit', '')))


def prune_builds(store, current):
    groups = ordinary_objects(store)
    indexes = {}
    for (run_id, attempt), files in groups.items():
        if INDEX in files:
            key = build_location(run_id, attempt) + INDEX
            if owned_build_object(store, key, run_id, attempt):
                indexes[run_id, attempt] = read_index(store, key, run_id, attempt)
    completed = sorted(
        (groups[run_id, attempt][INDEX], run_id, attempt) for run_id, attempt in indexes
    )
    kept = {(run_id, attempt) for _, run_id, attempt in completed[-KEEP_BUILDS:]}
    for (run_id, attempt), files in groups.items():
        if (run_id, attempt) in kept or (run_id, attempt) == current:
            continue
        prefix = build_location(run_id, attempt)
        if INDEX in files:
            index = indexes.get((run_id, attempt))
            if index is None:
                continue
            archives = {
                file['file']: object_metadata(index, file['sha256'])
                for file in index['files'].values()
            }
        else:
            archives = dict.fromkeys(files)
        keys = [prefix + name for name in sorted(archives.keys() & files.keys())
                if owned_build_object(store, prefix + name, run_id, attempt, expected=archives[name])]
        if keys:
            result = store.client.delete_objects(
                Bucket=store.bucket, Delete={'Objects': [{'Key': key} for key in keys]},
            )
            if result.get('Errors'):
                raise ValueError(f'Could not remove expired downloads: {result["Errors"]}')
        if INDEX in files:
            store.client.delete_object(Bucket=store.bucket, Key=prefix + INDEX)
        if keys or INDEX in files:
            print(f'Removed expired build downloads from {prefix}', flush=True)


def temporary_artifact_ids(repository, repository_id, index, publication_run, publication_sha):
    run_id, source_sha = index['run_id'], index['source_sha']
    artifacts = run_artifacts(repository, run_id)
    publication = artifacts if publication_run == run_id else run_artifacts(repository, publication_run)
    indexes = [artifact for artifact in publication if artifact['name'] == INDEX
               and not artifact['expired']
               and artifact_origin(artifact, publication_run, repository_id, publication_sha)]
    if len(indexes) != 1:
        raise ValueError('The Actions download list is missing or duplicated; keep the build archives.')
    temporary = []
    for artifact in artifacts:
        name = artifact['name']
        if not artifact_origin(artifact, run_id, repository_id, source_sha):
            continue
        if name in index['files'] and name != 'logs':
            expected = index['files'][name]
            if (artifact.get('digest') != f"sha256:{expected['sha256']}"
                    or artifact['size_in_bytes'] != expected['size']):
                raise ValueError(f'The Actions archive changed after publication: {name}')
            temporary.append(artifact['id'])
        elif name.startswith(('build-input-', 'build-logs-')) or LOG_ARCHIVE.fullmatch(name):
            temporary.append(artifact['id'])
    return temporary


def remove_temporary_artifacts(repository, artifact_ids):
    for artifact_id in artifact_ids:
        subprocess.run(
            ['gh', 'api', '--method', 'DELETE', f'/repos/{repository}/actions/artifacts/{artifact_id}'],
            check=True,
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('stage', 'publish-index', 'prune'))
    args = parser.parse_args()
    repository = os.environ['GITHUB_REPOSITORY']
    repository_id = int(os.environ['GITHUB_REPOSITORY_ID'])
    publication_run = int(os.environ['GITHUB_RUN_ID'])
    publication_sha = os.environ['GITHUB_SHA']
    source_run = os.environ.get('SOURCE_RUN')
    requested_sha = os.environ.get('SOURCE_SHA')
    if bool(source_run) != bool(requested_sha):
        raise ValueError('Supply both the source run and its commit.')
    run_id = int(source_run) if source_run else publication_run
    source_sha = requested_sha or publication_sha
    attempt_var = 'PUBLISHED_ATTEMPT' if args.action == 'prune' else 'GITHUB_RUN_ATTEMPT'
    attempt = int(os.environ[attempt_var])
    if (os.environ['GITHUB_REF'] != 'refs/heads/main' or run_id < 1 or attempt < 1
            or not re.fullmatch(r'[0-9a-f]{40}', source_sha)):
        raise ValueError('Publish downloads only for main Build products attempts.')
    run = None
    if source_run or args.action != 'publish-index':
        run = product_run(repository, repository_id, run_id, source_sha)
    if source_run:
        if args.action == 'prune' and run['run_attempt'] != attempt:
            raise ValueError('The source build attempt changed; keep its Actions archives.')
        attempt = run['run_attempt']
    index_path = Path(os.environ['RUNNER_TEMP']) / INDEX
    store = Store()
    if args.action == 'stage':
        if source_run:
            targets = recoverable_targets(repository, run)
        else:
            targets = json.loads(os.environ['ROOTFS_MATRIX'])['target']
        available = run_artifacts(repository, run_id)
        log_id = (latest_log_id(available, attempt) if source_run
                  else int(os.environ['LOG_ARTIFACT_ID']))
        artifacts = select_artifacts(
            available, artifact_names(targets), log_id, run_id, repository_id, source_sha,
        )
        index = make_index(artifacts, run_id, attempt, source_sha)
        stage_downloads(store, index_path, index, artifacts, repository)
    elif args.action == 'publish-index':
        publish_index(store, index_path, run_id, attempt, source_sha)
    else:
        index = read_index(store, build_location(run_id, attempt) + INDEX, run_id, attempt)
        if index['source_sha'] != source_sha:
            raise ValueError('The published downloads came from a different commit.')
        temporary = temporary_artifact_ids(
            repository, repository_id, index, publication_run, publication_sha,
        )
        prune_builds(store, (run_id, attempt))
        remove_temporary_artifacts(repository, temporary)


if __name__ == '__main__':
    main()
