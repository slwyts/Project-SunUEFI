import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import package_bsp as bsp


class BspPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def args(self, command='inspect', *extra):
        return bsp.parser().parse_args([command, *extra])

    def test_default_config_has_no_desktop_identity_or_native_payload(self):
        report = bsp.build(self.args())
        self.assertEqual(report['target'], {'distro': 'debian', 'suite': 'trixie', 'arch': 'aarch64'})
        self.assertEqual(report['package_architecture'], 'all')
        self.assertFalse(report['auto_enable_services'])
        self.assertFalse(report['device_tested'])
        self.assertEqual(set(report['required_layers']), {'runtime', 'modules', 'mesa', 'firmware'})
        for name in report['payload_files']:
            self.assertFalse(any(token in name for token in ('fstab', 'ssh', 'machine-id', 'gdm3', '80-drivers', 'libinput')))
        self.assertFalse(any('wireplumber' in p for p in report['payload_files']))
        row=report['payload_files']['etc/modules-load.d/piano-bluetooth.conf']
        self.assertEqual(row['source_component'],'Project SunUEFI')
        data=(bsp.BSP/'common/etc/modules-load.d/piano-bluetooth.conf').read_text()
        self.assertEqual([line for line in data.splitlines()if line and not line.startswith('#')],['uhid'])
        camera = bsp.build(self.args('inspect', '--feature', 'camera'))
        self.assertIn('etc/wireplumber/wireplumber.conf.d/50-piano-camera.conf', camera['payload_files'])
        self.assertIn('wireplumber (>= 0.5)', camera['depends'])

    def test_deepin_requires_explicit_suite(self):
        with self.assertRaisesRegex(ValueError, 'explicit'):
            bsp.build(self.args('inspect', '--target', 'deepin'))
        report = bsp.build(self.args('inspect', '--target', 'deepin', '--suite', 'user-selected-suite'))
        self.assertEqual(report['target']['suite'], 'user-selected-suite')
        self.assertFalse(report['dependency_resolution_tested'])

    def test_payload_tamper_refused_before_stage(self):
        copied = self.base / 'bsp'
        shutil.copytree(bsp.BSP, copied, symlinks=True)
        (copied / 'common/etc/piano/display.conf').write_text('tampered\n')
        args = self.args('stage', '--output', str(self.base / 'stage'))
        with patch.object(bsp, 'BSP', copied):
            with self.assertRaisesRegex(ValueError, 'payload drift'):
                bsp.build(args)
        self.assertFalse(args.output.exists())

    def test_tracked_ucm_patch_applies_to_exact_public_source(self):
        source = ROOT / 'upstream/debian-piano-current'
        if not (source / '.git').exists():
            self.skipTest('Pinned public source is not initialized')
        row = next(r for r in json.loads((bsp.BSP / 'manifest.json').read_text())['files'] if r['path'].endswith('/HiFi.conf'))
        commit = json.loads((bsp.BSP / 'manifest.json').read_text())['source']['commit']
        raw = subprocess.check_output(['git', '-C', source, 'show', commit + ':' + row['source']])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), row['source_sha256'])
        path = self.base / row['source']
        path.parent.mkdir(parents=True)
        path.write_bytes(raw)
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', bsp.BSP / row['patch']], cwd=self.base, check=True, capture_output=True)
        self.assertEqual(path.read_bytes(), (bsp.BSP / 'common' / row['path']).read_bytes())
        self.assertIn(b"VA DMIC MUX0' DMIC1", path.read_bytes())
        self.assertIn(b'CaptureChannels 2', path.read_bytes())

    def test_dpkg_file_owner_and_unowned_existing_file_conflicts(self):
        root = self.base / 'guest'
        root.mkdir()
        name = 'usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf'
        path = root / name
        path.parent.mkdir(parents=True)
        path.write_text('existing target configuration')
        args = self.args('inspect', '--against', str(root))
        with self.assertRaisesRegex(ValueError, 'unowned-existing-file'):
            bsp.build(args)
        info = root / 'var/lib/dpkg/info'
        info.mkdir(parents=True)
        (info / 'alsa-ucm-conf.list').write_text('/' + name + '\n')
        with self.assertRaisesRegex(ValueError, 'alsa-ucm-conf'):
            bsp.build(args)
        (info / 'alsa-ucm-conf.list').unlink()
        (info / 'piano-device-config.list').write_text('/' + name + '\n')
        self.assertTrue(bsp.build(args)['conflict_check']['performed'])
        self.assertEqual(path.read_text(), 'existing target configuration')

    def test_arch_ownership_conflict(self):
        root = self.base / 'guest'
        db = root / 'var/lib/pacman/local/alsa-ucm-conf-1.2.0-1'
        db.mkdir(parents=True)
        name = 'usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf'
        (db / 'files').write_text('%FILES%\n' + name + '\n\n%BACKUP%\n')
        with self.assertRaisesRegex(ValueError, 'alsa-ucm-conf'):
            bsp.build(self.args('inspect', '--target', 'arch', '--against', str(root)))

    @unittest.skipUnless(shutil.which('tar'), 'tar unavailable')
    def test_real_tar_and_arch_recipe_metadata(self):
        out = self.base / 'bundle'
        report = bsp.build(self.args('package', '--format', 'tar', '--target', 'arch', '--archrecipe', '--output', str(out)))
        self.assertFalse(report['package_ready'])
        self.assertFalse((out / '.incomplete').exists())
        self.assertIn('arch=(any)', (out / 'PKGBUILD').read_text())
        with tarfile.open(out / 'payload.tar') as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 0))
            member = archive.getmember('./usr/share/alsa/ucm2/conf.d/sm8750/Xiaomi Pad 8 Pro.conf')
            self.assertTrue(member.issym())
            self.assertEqual(member.linkname, '../../Qualcomm/sm8750/Xiaomi-Pad-8-Pro/Xiaomi-Pad-8-Pro.conf')
            hifi = archive.extractfile('./usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf').read()
            self.assertIn(b"VA_DEC0 Volume' 84", hifi)
        self.assertEqual(report['files']['payload.tar']['sha256'], hashlib.sha256((out / 'payload.tar').read_bytes()).hexdigest())

    def test_missing_deb_backend_does_not_fake_package(self):
        out = self.base / 'bundle'
        with patch.object(bsp.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'Missing package backend'):
                bsp.build(self.args('package', '--format', 'deb', '--output', str(out)))
        self.assertFalse(out.exists())

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable; no fake deb test')
    def test_real_deb_control_and_payload(self):
        out = self.base / 'deb'
        report = bsp.build(self.args('package', '--format', 'deb', '--output', str(out)))
        artifact = out / next(iter(report['files']))
        field = subprocess.check_output(['dpkg-deb', '--field', artifact, 'Architecture'], text=True).strip()
        self.assertEqual(field, 'all')
        data = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', artifact])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid), (0, 0))
            self.assertIn('./etc/modprobe.d/piano.conf', archive.getnames())
        control = self.base / 'control'
        subprocess.run(['dpkg-deb', '--control', artifact, control], check=True)
        guard = (control / 'preinst').read_text()
        self.assertIn('dpkg-query', guard)
        self.assertNotIn('systemctl', guard)
        self.assertNotIn('Replaces:', (control / 'control').read_text())


if __name__ == '__main__':
    unittest.main()
