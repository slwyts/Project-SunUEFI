"""Exercise the real FS/GPFS callback and POSIX I/O without opening hardware."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        mink = ROOT / 'build/source-cache/minkipc'
        includes = [
            mink / 'libminkadaptor/include',
            mink / 'listeners/libfsservice/fs',
            mink / 'listeners/libfsservice/gpfs',
            mink / 'listeners/libfsservice/cmn',
        ]
        if not all(path.is_dir() for path in includes):
            raise unittest.SkipTest('Cached public MinkIPC filesystem headers are required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-storage-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_storage'
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-Wl,--wrap=malloc', '-Wl,--wrap=ioctl', '-Wl,--wrap=fsync',
            *[f'-I{path}' for path in includes],
            str(ROOT / 'tests/native/test_goodix_storage.c'),
            '-pthread', '-o', str(cls.binary),
        ], check=True)

    def check_files(self, name):
        with tempfile.TemporaryDirectory(prefix='piano-goodix-files-') as directory:
            os.chmod(directory, 0o700)
            result = subprocess.run([str(self.binary), name, directory],
                                    capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_offset_writes_backup_and_reopen(self):
        self.check_files('writes')

    def test_gpfs_message_payload_and_multi_message_file_boundaries(self):
        self.check_files('large-files')

    def test_profile_rename_remove_and_diagnostic_restrictions(self):
        self.check_files('names')

    def test_paths_links_file_types_and_packet_bounds(self):
        self.check_files('boundaries')

    def test_private_handles_errno_and_exclusive_directory_lock(self):
        self.check_files('handles')

    def test_failed_staging_preserves_committed_files_and_backup(self):
        self.check_files('failures')

    def test_rpmb_probe_refuses_every_command_without_copying_frames(self):
        self.check_files('rpmb')

    def test_rpmb_validation_and_submission_use_the_same_private_snapshot(self):
        result = self.check_files('snapshot')
        self.assertIn('RPMB response first_type=0x300', result.stderr)
        for private in ('PIANO_PRIVATE', '5049414e4f5f50524956415445',
                        '987654321', '3ade68b1'):
            self.assertNotIn(private, result.stdout + result.stderr)

    def test_rpmb_each_ioctl_error_and_short_transfer_stops_the_session(self):
        result = self.check_files('rpmb-failures')
        for stage in ('write-request', 'result-request', 'response'):
            self.assertIn(f'RPMB SG stage={stage} ioctl=-1 errno=5', result.stderr)
        self.assertIn('din_resid=1', result.stderr)
        self.assertIn('dout_resid=1', result.stderr)

    def test_fs_gpfs_and_rpmb_share_one_shutdown_and_cleared_memory_lifetime(self):
        self.check_files('listeners')

    def test_failed_partial_registration_waits_for_its_remote_callback(self):
        self.check_files('listeners-partial')


if __name__ == '__main__':
    unittest.main()
