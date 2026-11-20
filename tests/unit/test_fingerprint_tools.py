"""Check the optional package with the real ARM64 compiler and CLI."""
import io
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_fingerprint as fingerprint
from package import fingerprint_tools
import package_bsp


class FingerprintToolsTests(unittest.TestCase):
    def test_builds_a_real_deb_without_a_desktop_dependency(self):
        arm = platform.machine() in ('arm64', 'aarch64')
        tools = ('git', 'tar', 'patch', 'dpkg-deb',
                 *(('cc',) if arm else ('aarch64-linux-gnu-gcc', 'qemu-aarch64-static')))
        if not all(shutil.which(name) for name in tools):
            self.skipTest('ARM64 compiler, execution and Debian package tools required')
        uapi = ROOT / 'build/runtime/uapi'
        if not (uapi / 'include/linux/tee.h').is_file():
            self.skipTest('Current exported release headers required')
        policy = json.loads((ROOT / 'config/fingerprint.json').read_text())
        if not all((ROOT / 'build/source-cache' / name / '.git').exists()
                   for name in policy['sources']):
            self.skipTest('Cached public fingerprint sources required')
        with tempfile.TemporaryDirectory(prefix='fingerprint-tools-package-') as directory:
            output = Path(directory) / 'debian'
            opts = package_bsp.parser().parse_args(['package', '--target', 'debian', '--format', 'deb',
                                                   '--fingerprint-tools', str(uapi), '--output', str(output)])
            report = package_bsp.build(opts)
            archive = output / next(iter(report['files']))
            fields = subprocess.check_output(['dpkg-deb', '-f', archive, 'Package', 'Architecture',
                                              'Depends', 'Recommends'], text=True).splitlines()
            self.assertEqual(fields, ['Package: piano-fingerprint-tools', 'Architecture: arm64'])
            contents = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', archive])
            with tarfile.open(fileobj=io.BytesIO(contents)) as members:
                binary = './usr/bin/piano-fingerprint-ctl'
                self.assertEqual(members.getmember(binary).mode, 0o755)
                self.assertEqual(members.extractfile(binary).read(),
                                 (output / 'payload' / binary).read_bytes())
                paths = {member.name.removeprefix('./') for member in members if member.isfile()}
                self.assertEqual(paths, {*report['payload_files'],
                                         fingerprint_tools.DOCS + 'bsp-manifest.json'})
                self.assertEqual(members.extractfile('./' + fingerprint_tools.DOCS + 'linux-tee.h').read(),
                                 (uapi / 'include/linux/tee.h').read_bytes())
            executable = output / 'payload/usr/bin/piano-fingerprint-ctl'
            command = ([] if arm else [shutil.which('qemu-aarch64-static')]) + [executable, '--help']
            self.assertIn('Usage: piano-fingerprint-ctl', subprocess.check_output(command, text=True, timeout=10))
            self.assertEqual(report['source']['client_sha256'], fingerprint.digest(fingerprint.CLI_SOURCE))


if __name__ == '__main__':
    unittest.main()
