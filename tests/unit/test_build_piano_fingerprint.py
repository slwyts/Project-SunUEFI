"""Check that the release backend archive stays source-only."""
import json
from pathlib import Path
import shutil
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
        tools = ('git', 'tar', 'patch')
        if not all(shutil.which(name) for name in tools):
            raise unittest.SkipTest('Git, tar and patch are required')
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
        cls.record = fingerprint.prepare_desktop_backend(cls.runtime, cls.uapi)

    def test_backend_archive_is_source_only_and_matches_native_code(self):
        archive = fingerprint.backend_sources(self.runtime)
        self.assertFalse((archive.parent / 'libpiano-goodix.a').exists())
        self.assertFalse((archive.parent / 'piano-fingerprint-ctl').exists())
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
