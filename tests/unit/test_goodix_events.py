"""Compile and execute the real Goodix event code without biometric hardware."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixEventsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-events-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_events'
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            str(ROOT / 'linux/userspace/fingerprint/piano-goodix-events.c'),
            str(ROOT / 'tests/native/test_goodix_events.c'), '-o', str(cls.binary),
        ], check=True)

    def check_exchange(self, name):
        result = subprocess.run([str(self.binary), name],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_capture_preserves_packet_and_advances_once(self):
        self.check_exchange('capture')

    def test_retry_clears_phase_upper_word_and_preserves_capture_progress(self):
        self.check_exchange('retry')

    def test_navigation_clears_only_its_result_range(self):
        self.check_exchange('navigation')

    def test_failures_and_pending_retry_can_be_cancelled(self):
        self.check_exchange('errors')

    def test_authorized_consumer_pauses_results_and_drains_extended_capture(self):
        self.check_exchange('authorized')

    def test_quality_retry_nomatch_and_fatal_statuses_are_distinct(self):
        self.check_exchange('quality')


if __name__ == '__main__':
    unittest.main()
