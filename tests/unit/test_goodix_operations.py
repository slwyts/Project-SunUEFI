"""Run the real operation lifecycle and shared event code without hardware."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixOperationsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-operations-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_operations'
        subprocess.run([
            *compiler, '-std=c17', '-D_DEFAULT_SOURCE', '-O2',
            '-Wall', '-Wextra', '-Werror',
            str(ROOT / 'linux/userspace/fingerprint/piano-goodix-events.c'),
            str(ROOT / 'linux/userspace/fingerprint/piano-goodix-operations.c'),
            str(ROOT / 'tests/native/test_goodix_operations.c'),
            '-o', str(cls.binary),
        ], check=True)

    def check_operation(self, name):
        result = subprocess.run([str(self.binary), name],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_enrollment_requires_save_then_post_enroll(self):
        self.check_operation('enroll')

    def test_authentication_requires_signed_identity_then_finish(self):
        self.check_operation('authenticate')

    def test_other_group_template_sid_and_challenge_never_authenticate(self):
        self.check_operation('identity')

    def test_cancellation_is_separate_from_durable_template_commit(self):
        self.check_operation('cancel')

    def test_enrollment_challenge_namespace_and_packet_capacity_are_required(self):
        self.check_operation('validation')

    def test_bad_acquisitions_nomatch_internal_retry_and_invalid_hat_stay_distinct(self):
        self.check_operation('quality')


if __name__ == '__main__':
    unittest.main()
