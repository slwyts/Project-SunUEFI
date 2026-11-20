"""Compile the actual ARM64 CLI and inspect its source-only libfprint inputs."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_fingerprint as fingerprint


class BuildPianoFingerprintTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tools = ('aarch64-linux-gnu-gcc', 'qemu-aarch64-static', 'git', 'tar', 'patch')
        if not all(shutil.which(name) for name in tools):
            raise unittest.SkipTest('The ARM64 compiler, qemu, git, tar and patch are required')
        config = json.loads((ROOT / 'config/fingerprint.json').read_text())
        for name in config['sources']:
            if not (ROOT / 'build/source-cache' / name / '.git').exists():
                raise unittest.SkipTest('Cached public fingerprint sources are required')
        temporary = tempfile.TemporaryDirectory(prefix='fingerprint-source-build-', dir=ROOT / 'build')
        cls.addClassCleanup(temporary.cleanup)
        cls.runtime = Path(temporary.name)
        cls.uapi = ROOT / 'build/runtime/uapi'
        if not (cls.uapi / 'include/linux/tee.h').is_file():
            raise unittest.SkipTest('Exported product kernel headers are required')
        fallback = cls.runtime / 'sysroot-headers/linux'
        fallback.mkdir(parents=True)
        (fallback / 'tee.h').write_text('#error The sysroot TEE header must not be used\n')
        cls.files, cls.record = fingerprint.build(cls.runtime, [
            shutil.which('aarch64-linux-gnu-gcc'), '-isystem', str(fallback.parent),
            '-O2', '-Wall', '-Wextra', '-Werror',
        ], cls.uapi, emulator=shutil.which('qemu-aarch64-static'))

    def test_backend_archive_is_source_only_and_matches_native_code(self):
        archive = fingerprint.backend_sources(self.runtime)
        self.assertFalse((archive.parent / 'libpiano-goodix.a').exists())
        with tarfile.open(archive) as sources:
            for member in sources.getmembers():
                self.assertNotIn(Path(member.name).suffix, ('.o', '.a', '.so'))
            for name in (*fingerprint.CORE_SOURCES, *fingerprint.HEADERS):
                self.assertEqual(sources.extractfile('./' + name).read(),
                                 (fingerprint.NATIVE / name).read_bytes())
            transport = sources.extractfile('./piano-goodix-tee.c').read()
            self.assertIn(b'TEE_IOC_VERSION', transport)
            self.assertIn(b'TEE_GEN_CAP_OBJREF', transport)
            self.assertEqual(sources.extractfile('./uapi/linux/tee.h').read(),
                             (self.uapi / 'include/linux/tee.h').read_bytes())
            self.assertEqual(self.record['uapi_headers']['linux/tee.h'],
                             fingerprint.digest(self.uapi / 'include/linux/tee.h'))

    def test_runtime_cli_is_arm64_and_help_does_not_open_a_device(self):
        binary = self.runtime / self.files['usr/local/sbin/piano-fingerprint-ctl']['file']
        data = binary.read_bytes()
        self.assertEqual(data[:6], b'\x7fELF\x02\x01')
        self.assertEqual(data[18:20], b'\xb7\x00')
        result = subprocess.run([shutil.which('qemu-aarch64-static'), binary, '--help'],
                                capture_output=True, text=True, check=True, timeout=10)
        self.assertIn('piano-fingerprint-ctl', result.stdout)
        notice = self.files['usr/share/licenses/piano-fingerprint/linux-tee.h']['file']
        self.assertEqual((self.runtime / notice).read_bytes(),
                         (self.uapi / 'include/linux/tee.h').read_bytes())

    def test_changed_archive_or_stale_build_record_is_rejected(self):
        archive = fingerprint.backend_sources(self.runtime)
        original = archive.read_bytes()
        try:
            archive.write_bytes(original + b'changed')
            with self.assertRaisesRegex(ValueError, 'differs'):
                fingerprint.backend_sources(self.runtime)
        finally:
            archive.write_bytes(original)
        marker = self.runtime / 'fingerprint/fingerprint.json'
        original_record = marker.read_bytes()
        changed = json.loads(original_record)
        changed['inputs']['config_sha256'] = 'stale'
        try:
            marker.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'Rebuild'):
                fingerprint.backend_sources(self.runtime)
        finally:
            marker.write_bytes(original_record)


if __name__ == '__main__':
    unittest.main()
