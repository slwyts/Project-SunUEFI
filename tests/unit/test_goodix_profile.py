"""Run the real Linux fingerprint profile code against private temporary files."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        includes = [
            ROOT / 'build/source-cache/minkipc/libminkadaptor/include',
            ROOT / 'build/source-cache/minkipc/listeners/libfsservice/fs',
            ROOT / 'build/source-cache/minkipc/listeners/libfsservice/gpfs',
            ROOT / 'build/source-cache/minkipc/listeners/libfsservice/cmn',
            ROOT / 'build/source-cache/qcbor/inc',
        ]
        if not all(path.is_dir() for path in includes):
            raise unittest.SkipTest('Cached public MinkIPC and QCBOR headers are required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-profile-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_profile'
        qcbor = ROOT / 'build/source-cache/qcbor/src'
        sources = [qcbor / name for name in (
            'UsefulBuf.c', 'qcbor_encode.c', 'qcbor_decode.c',
            'qcbor_err_to_str.c', 'ieee754.c',
        )]
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            *[f'-I{path}' for path in includes],
            str(ROOT / 'tests/native/test_goodix_profile.c'),
            *map(str, sources), '-pthread', '-lm', '-o', str(cls.binary),
        ], check=True)

    def check_profile(self, name):
        with tempfile.TemporaryDirectory(prefix='piano-profile-files-') as directory:
            os.chmod(directory, 0o700)
            result = subprocess.run([str(self.binary), name, directory],
                                    capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_private_metadata_roundtrip(self):
        self.check_profile('roundtrip')

    def test_recovery_commits_only_recorded_id_in_selected_group(self):
        self.check_profile('recovery')

    def test_identity_links_permissions_and_malformed_state_are_rejected(self):
        self.check_profile('boundaries')

    def test_failed_atomic_replacement_preserves_previous_profile(self):
        self.check_profile('failures')

    def test_failed_initialization_can_reopen_empty_profile(self):
        self.check_profile('initialization-failure')

    def test_signal_and_synchronized_callback_cancellation(self):
        self.check_profile('cancellation')

    def test_only_selected_linux_gatekeeper_create_verify_opens_rpmb_window(self):
        self.check_profile('gatekeeper-window')

    def test_initialization_uses_policy_saved_before_init_overwrites_payload(self):
        self.check_profile('initialization-policy')


if __name__ == '__main__':
    unittest.main()
