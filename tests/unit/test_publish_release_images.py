"""Check conditional R2 copies and public range verification."""
from pathlib import Path
from subprocess import CompletedProcess
import sys
import unittest
from unittest.mock import Mock, call, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.github/scripts'))
import publish_release_images as release


class ReleaseImagesTests(unittest.TestCase):
    def test_copy_completes_conditionally_without_unsupported_copy_conditions(self):
        src = {'Bucket': 'images', 'Key': 'builds/10/1/SunUEFI-ESP.zip'}
        dest = {'Bucket': 'images', 'Key': 'releases/v1.0.0/SunUEFI-ESP.zip'}
        metadata = {'sha256': 'ab' * 32, 'source-commit': 'cd' * 20}
        part_size = 8 * 1024 ** 2
        cases = (
            (4096, [[]]),
            (2 * part_size + 4, [
                [('CopySourceRange', f'bytes=0-{part_size - 1}')],
                [('CopySourceRange', f'bytes={part_size}-{2 * part_size - 1}')],
                [('CopySourceRange', f'bytes={2 * part_size}-{2 * part_size + 3}')],
            ]),
        )
        for size, extents in cases:
            with self.subTest(size=size), patch.object(release, 'PART_BYTES', part_size):
                client = Mock()
                client.head_object.return_value = {'ContentLength': size, 'Metadata': metadata}
                client.create_multipart_upload.return_value = {'UploadId': 'origin'}
                client.upload_part_copy.side_effect = [
                    {'CopyPartResult': {'ETag': str(i)}} for i in range(len(extents))
                ]
                release.copy_zip(client, src, dest, size, metadata)
                client.head_object.assert_called_once_with(**src)
                self.assertEqual(client.upload_part_copy.call_args_list, [
                    call(**dest, UploadId='origin', CopySource=src, PartNumber=i + 1, **dict(extent))
                    for i, extent in enumerate(extents)
                ])
                client.complete_multipart_upload.assert_called_once_with(
                    **dest, UploadId='origin', IfNoneMatch='*', MultipartUpload={
                        'Parts': [{'ETag': str(i), 'PartNumber': i + 1} for i in range(len(extents))],
                    },
                )

    @patch.object(release.time, 'sleep')
    @patch.object(release.subprocess, 'run')
    def test_range_retries_without_downloading_the_whole_file(self, run, sleep):
        run.side_effect = (
            CompletedProcess([], 63, '200\n\n0\n', 'Maximum file size exceeded'),
            CompletedProcess([], 0, '206\nbytes 4095-4095/4096\n1\n', ''),
        )
        release.verify_download('https://example.com/image.zip', 4096)
        self.assertEqual(run.call_count, 2)
        for probe in run.call_args_list:
            command = probe.args[0]
            self.assertEqual(command[command.index('--range') + 1], '4095-4095')
            self.assertEqual(command[command.index('--max-filesize') + 1], '1')
        sleep.assert_called_once()

    @patch.object(release.time, 'sleep')
    @patch.object(release.subprocess, 'run')
    def test_range_rejects_wrong_content_length_after_retries(self, run, sleep):
        run.return_value = CompletedProcess([], 0, '206\nbytes 4095-4095/4097\n1\n', '')
        with self.assertRaisesRegex(ValueError, r'received.*4095-4095/4097'):
            release.verify_download('https://example.com/image.zip', 4096)
        self.assertEqual(run.call_count, len(release.RANGE_RETRY_DELAYS) + 1)
        self.assertEqual(sleep.call_count, len(release.RANGE_RETRY_DELAYS))


if __name__ == '__main__':
    unittest.main()
