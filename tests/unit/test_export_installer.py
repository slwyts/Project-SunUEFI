import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import export_installer as exporter


class InstallerExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='installer tests ')
        self.base = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_uefi_only_keeps_exact_installer_without_disk_manifest(self):
        product = self.base / 'source/PianoUEFI-product.img'
        product.parent.mkdir()
        product.write_bytes(b'ANDROID! TEST FIXTURE ONLY')
        out = self.base / 'download with spaces'
        record = exporter.export(out, product=product)
        self.assertEqual((out / 'install_piano.py').read_bytes(), (ROOT / 'tools/install_piano.py').read_bytes())
        self.assertEqual(record['kind'], 'uefi-utility')
        self.assertIsNone(record['bundle'])
        self.assertFalse(record['fresh_partition_install_ready'])
        self.assertFalse(record['recovery_install_ready'])
        self.assertFalse((out / 'manifest.json').exists())
        self.assertFalse((out / 'bundle').exists())
        self.assertFalse((out / '.incomplete').exists())

    def test_launcher_runs_outside_repo_and_supports_space_paths(self):
        out = self.base / 'download with spaces'
        exporter.export(out)
        cwd = self.base / 'unrelated working directory'
        cwd.mkdir()
        env = {**os.environ, 'SUNUEFI_PYTHON': sys.executable}
        if os.name == 'nt':
            argv = ['cmd', '/d', '/c', str(out / 'install.cmd'), '--help']
        else:
            (out / 'install.sh').chmod(0o644)
            argv = ['sh', str(out / 'install.sh'), '--help']
        result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('inspect', result.stdout)
        self.assertIn('--execute', result.stdout)
        self.assertEqual(list(cwd.iterdir()), [])

    @unittest.skipIf(os.name == 'nt', 'POSIX PATH fixture')
    def test_missing_platform_tools_fails_before_device_call(self):
        out = self.base / 'download'
        exporter.export(out)
        commands = self.base / 'minimal path'
        commands.mkdir()
        (commands / 'dirname').symlink_to(shutil.which('dirname'))
        env = {**os.environ, 'SUNUEFI_PYTHON': sys.executable, 'PATH': str(commands)}
        result = subprocess.run([str(out / 'install.sh'), '--serial', 'fixture'], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('adb, fastboot', result.stderr)

    def test_modified_installer_is_not_executed(self):
        out = self.base / 'download'
        exporter.export(out)
        (out / 'install_piano.py').write_text('raise RuntimeError("should not execute")\n')
        result = subprocess.run([sys.executable, out / 'installer_launcher.py', '--help'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checksum mismatch', result.stderr)
        self.assertNotIn('should not execute', result.stderr)

    def test_existing_export_and_partial_bundle_refused(self):
        out = self.base / 'download'
        exporter.export(out)
        with self.assertRaisesRegex(ValueError, 'fresh'):
            exporter.export(out)
        bundle = self.base / 'bundle'
        bundle.mkdir()
        (bundle / '.incomplete').touch()
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            exporter.export(self.base / 'other', bundle=bundle)
        self.assertFalse((self.base / 'other').exists())

    @unittest.skipUnless(shutil.which('mkfs.vfat') and shutil.which('mke2fs'), 'Real filesystem tools unavailable')
    def test_real_disk_bundle_and_automatic_absolute_bundle_path(self):
        bundle = self.base / 'source bundle'
        bundle.mkdir()
        for name in ('esp.img', 'root.ext4.img'):
            with (bundle / name).open('wb') as stream:
                stream.truncate(64 * 1024 * 1024)
        subprocess.run(['mkfs.vfat', '-F', '32', '-n', 'SUNUEFI_ESP', bundle / 'esp.img'], check=True, capture_output=True)
        subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-L', 'PIANOROOT', bundle / 'root.ext4.img'], check=True, capture_output=True)
        manifest = {'schema_version': 1, 'status': 'HOST_BUILT_NOT_DEVICE_READY', 'root_policy': 'LABEL=PIANOROOT',
                    'files': {p.name: {'sha256': exporter.digest(p), 'bytes': p.stat().st_size}
                              for p in bundle.iterdir()}}
        (bundle / 'manifest.json').write_text(json.dumps(manifest))
        out = self.base / 'download with spaces'
        exporter.export(out, bundle=bundle)
        self.assertEqual((out / 'bundle/manifest.json').read_bytes(), (bundle / 'manifest.json').read_bytes())
        spec = importlib.util.spec_from_file_location('exported_launcher_fixture', out / 'installer_launcher.py')
        launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(launcher)
        cwd = os.getcwd()
        with patch.object(launcher.sys, 'argv', ['launcher', 'plan', '--serial', 'plan', '--output', 'relative plan.json']), \
             patch.object(launcher.shutil, 'which', return_value='/fixture/tool'), \
             patch.object(launcher.subprocess, 'call', return_value=0) as call:
            self.assertEqual(launcher.main(), 0)
        forwarded = call.call_args.args[0]
        self.assertEqual(forwarded[-2:], ['--bundle', str(out / 'bundle')])
        self.assertIn('relative plan.json', forwarded)
        self.assertEqual(os.getcwd(), cwd)


if __name__ == '__main__':
    unittest.main()
