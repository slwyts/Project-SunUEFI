"""Check the real patched QTEE root setup without opening a TEE device."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_fingerprint as fingerprint


class QcomteeObjectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]) or not shutil.which('patch'):
            raise unittest.SkipTest('A native C compiler and patch are required')
        source = ROOT / 'build/source-cache/quic-teec'
        if not (source / 'libqcomtee/src/qcomtee_object.c').is_file():
            raise unittest.SkipTest('The cached public quic-teec source is required')
        record = json.loads((ROOT / 'config/fingerprint.json').read_text())['sources']['quic-teec']
        temporary = tempfile.TemporaryDirectory(prefix='qcomtee-object-')
        cls.addClassCleanup(temporary.cleanup)
        work = Path(temporary.name)
        archive = subprocess.run(['git', '-C', source, 'archive', record['commit']],
                                 capture_output=True, check=True).stdout
        subprocess.run(['tar', '-xf', '-', '-C', work], input=archive, check=True)
        for path, expected in fingerprint.patches(record):
            if fingerprint.digest(path) != expected:
                raise ValueError('QTEE patch checksum differs: ' + str(path))
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path],
                           cwd=work, check=True, capture_output=True)
        cls.binary = work / 'test_qcomtee_object'
        subprocess.run([
            *compiler, '-std=gnu17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            f'-I{work}/libqcomtee/include', f'-I{work}/libqcomtee/src',
            str(work / 'libqcomtee/src/qcomtee_object.c'),
            str(ROOT / 'tests/native/test_qcomtee_object.c'),
            '-pthread', '-o', str(cls.binary),
        ], check=True)

    def test_capabilities_descriptor_cleanup_and_transport_errors(self):
        with tempfile.TemporaryDirectory(prefix='qcomtee-root-') as directory:
            result = subprocess.run([str(self.binary), str(Path(directory) / 'tee-file')],
                                    capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
