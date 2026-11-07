"""Check real Gatekeeper CBOR and Linux credential transactions without QTEE."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GoodixGatekeeperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        qcbor = ROOT / 'build/source-cache/qcbor'
        if not (qcbor / 'inc/qcbor/qcbor_decode.h').is_file():
            raise unittest.SkipTest('The cached public QCBOR source is required')
        temporary = tempfile.TemporaryDirectory(prefix='piano-goodix-gatekeeper-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'test_goodix_gatekeeper'
        sources = [qcbor / 'src' / name for name in (
            'UsefulBuf.c', 'qcbor_encode.c', 'qcbor_decode.c',
            'qcbor_err_to_str.c', 'ieee754.c',
        )]
        subprocess.run([
            *compiler, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror',
            f'-I{qcbor}/inc',
            str(ROOT / 'linux/userspace/fingerprint/piano-goodix-gatekeeper.c'),
            str(ROOT / 'tests/native/test_goodix_gatekeeper.c'),
            *map(str, sources), '-lm', '-o', str(cls.binary),
        ], check=True)

    def check_protocol(self, name):
        result = subprocess.run([str(self.binary), name],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_credential_persists_and_hat_retains_factory_byte_order(self):
        self.check_protocol('lifecycle')

    def test_pending_provisioning_and_retry_preserve_linux_secret(self):
        self.check_protocol('retry')

    def test_bad_frames_macs_challenges_and_statuses_do_not_return_hat(self):
        self.check_protocol('protocol')

    def test_namespace_permissions_corruption_and_symlinks_fail_closed(self):
        self.check_protocol('storage')


if __name__ == '__main__':
    unittest.main()
