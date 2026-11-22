"""Build download selection and retention keep the requested product snapshots."""
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.github/scripts'))
import publish_build_downloads as downloads


COMMIT = 'ad' * 20
DIGEST = 'b9' * 32
ORIGIN = {
    'id': 100, 'repository_id': 5, 'head_repository_id': 5,
    'head_branch': 'main', 'head_sha': COMMIT,
}


class BuildBucket:
    bucket = 'test-downloads'

    def __init__(self):
        self.client = self
        self.objects = {}
        self.fail_delete = None

    def add_object(self, key, time, metadata=None, data=b''):
        self.objects[key] = (time, metadata or {}, data)

    def get_paginator(self, operation):
        if operation != 'list_objects_v2':
            raise AssertionError(operation)
        return self

    def paginate(self, *, Bucket, Prefix):
        if Bucket != self.bucket or Prefix != 'builds/':
            raise AssertionError((Bucket, Prefix))
        objects = [{'Key': key, 'LastModified': time}
                   for key, (time, _, _) in sorted(self.objects.items()) if key.startswith(Prefix)]
        for start in range(0, len(objects), 4):
            yield {'Contents': objects[start:start + 4]}

    def head_object(self, *, Bucket, Key):
        return {'Metadata': self.objects[Key][1]}

    def get_object(self, *, Bucket, Key):
        _, metadata, data = self.objects[Key]
        return {'Body': BytesIO(data), 'Metadata': metadata}

    def delete_objects(self, *, Bucket, Delete):
        if any(obj['Key'] == self.fail_delete for obj in Delete['Objects']):
            return {'Errors': [{'Key': self.fail_delete, 'Code': 'AccessDenied'}]}
        for obj in Delete['Objects']:
            del self.objects[obj['Key']]
        return {}

    def delete_object(self, *, Bucket, Key):
        del self.objects[Key]

    def build(self, run, attempt, time):
        prefix = f'builds/{run}/{attempt}/'
        metadata = {
            'build-run': str(run), 'build-attempt': str(attempt), 'source-commit': COMMIT,
        }
        files = {}
        for name in ('SunUEFI-Module', 'SunUEFI-UEFI-Fastboot', 'logs'):
            filename = f'{name}.zip'
            self.add_object(prefix + filename, time - 1, dict(metadata, sha256=DIGEST))
            files[name] = {
                'file': filename, 'size': 500, 'sha256': DIGEST,
                'url': f'https://sunuefi-repo.okassets.uk/{prefix}{filename}',
            }
        index = {
            'schema_version': 1, 'storage': 'Cloudflare R2', 'source_sha': COMMIT,
            'run_id': run, 'run_attempt': attempt, 'files': files,
        }
        data = json.dumps(index).encode()
        self.add_object(prefix + 'downloads.json', time,
                        dict(metadata, sha256=hashlib.sha256(data).hexdigest()), data)


def github_artifact(name, artifact_id, **changes):
    return {
        'name': name, 'id': artifact_id, 'expired': False, 'size_in_bytes': 500,
        'digest': f'sha256:{DIGEST}', 'workflow_run': ORIGIN, **changes,
    }


class BuildDownloadsTests(unittest.TestCase):
    def test_single_distro_uses_the_selected_attempt_logs_and_exact_set(self):
        expected = downloads.artifact_names(['debian-denial'])
        self.assertEqual(expected, {'SunUEFI-ESP', 'SunUEFI-Module', 'SunUEFI-UEFI-Fastboot',
                                    'SunUEFI-Root-debian-denial', 'logs'})
        artifacts = [github_artifact(name, artifact_id)
                     for artifact_id, name in enumerate(sorted(expected - {'logs'}), 10)]
        artifacts.extend([github_artifact('logs-1', 1), github_artifact('logs-2', 2),
                          github_artifact('SunUEFI-Root-steamos', 3)])
        selected = downloads.select_artifacts(artifacts, expected, 2, 100, 5, COMMIT)
        self.assertEqual(set(selected), expected)
        self.assertEqual(selected['logs']['id'], 2)
        with self.assertRaisesRegex(ValueError, 'missing: logs'):
            downloads.select_artifacts(artifacts, expected, 3, 100, 5, COMMIT)
        with self.assertRaisesRegex(ValueError, 'Cannot verify'):
            downloads.select_artifacts(artifacts, expected, 2, 100, 5, '0' * 40)

    def test_index_cannot_point_at_another_attempt_or_at_a_release(self):
        archives = {name: github_artifact(name, artifact_id)
                    for artifact_id, name in enumerate(downloads.artifact_names([]), 1)}
        index = downloads.make_index(archives, 100, 2, COMMIT)
        self.assertTrue(downloads.valid_index(index, 100, 2))
        self.assertFalse(downloads.valid_index(index, 100, 3))
        self.assertEqual(index['files']['SunUEFI-Module']['url'],
                         'https://sunuefi-repo.okassets.uk/builds/100/2/SunUEFI-Module.zip')
        index['files']['logs']['url'] = 'https://sunuefi-repo.okassets.uk/releases/v1.0.0/logs.zip'
        self.assertFalse(downloads.valid_index(index, 100, 2))

    def test_prune_keeps_three_valid_attempts_by_completion_and_ignores_other_objects(self):
        store = BuildBucket()
        for run, attempt, time in ((100, 1, 10), (103, 1, 20), (102, 1, 30), (100, 2, 40)):
            store.build(run, attempt, time)
        failed = 'builds/104/1/SunUEFI-Module.zip'
        store.add_object(failed, 50, {'build-run': '104', 'build-attempt': '1',
                                     'source-commit': COMMIT, 'sha256': DIGEST})
        foreign = {'builds/104/1/SunUEFI-UEFI-Fastboot.zip', 'builds/999/1/downloads.json',
                   'builds/100/1/README.txt', 'releases/v1.0.0/SunUEFI-Module.zip',
                   'apt/pool/main/linux-piano.deb', 'builds/1000/01/logs.zip'}
        for key in foreign:
            store.add_object(key, 60)
        downloads.prune_builds(store, (100, 2))
        for prefix in ('builds/100/2/', 'builds/102/1/', 'builds/103/1/'):
            self.assertEqual(sum(key.startswith(prefix) for key in store.objects), 4)
        self.assertEqual({key for key in store.objects if key.startswith('builds/100/1/')},
                         {'builds/100/1/README.txt'})
        self.assertNotIn(failed, store.objects)
        self.assertTrue(foreign <= store.objects.keys())

    def test_prune_keeps_index_when_archive_deletion_fails(self):
        store = BuildBucket()
        for run in range(100, 104):
            store.build(run, 1, run)
        store.fail_delete = 'builds/100/1/SunUEFI-Module.zip'
        with self.assertRaisesRegex(ValueError, 'AccessDenied'):
            downloads.prune_builds(store, (103, 1))
        self.assertIn('builds/100/1/downloads.json', store.objects)

    def test_cleanup_needs_actions_index_and_wont_remove_new_artifact_bytes(self):
        module = github_artifact('SunUEFI-Module', 1)
        archives = {'SunUEFI-Module': module}
        index = downloads.make_index(archives, 100, 2, COMMIT)
        with patch.object(downloads, 'run_artifacts', return_value=[module]):
            with self.assertRaisesRegex(ValueError, 'download list is missing'):
                downloads.temporary_artifact_ids('owner/repo', 5, index)
        keep = github_artifact('downloads.json', 2)
        different = dict(module, digest='sha256:' + '0' * 64)
        with patch.object(downloads, 'run_artifacts', return_value=[keep, different]):
            with self.assertRaisesRegex(ValueError, 'changed after publication'):
                downloads.temporary_artifact_ids('owner/repo', 5, index)
        objects = [keep, module, github_artifact('build-input-kernel', 3),
                   github_artifact('logs-2', 4), github_artifact('extra', 5),
                   github_artifact('logs-3', 6, workflow_run=dict(ORIGIN, id=101))]
        with patch.object(downloads, 'run_artifacts', return_value=objects):
            self.assertEqual(downloads.temporary_artifact_ids('owner/repo', 5, index), [1, 3, 4])


if __name__ == '__main__':
    unittest.main()
