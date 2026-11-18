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
        self.assertEqual(report['target'], {'distro': 'debian', 'suite': 'sid', 'arch': 'aarch64'})
        self.assertEqual(report['package_architecture'], 'all')
        self.assertFalse(report['auto_enable_services'])
        self.assertFalse(report['device_tested'])
        self.assertEqual(set(report['required_layers']), {'runtime', 'modules', 'mesa', 'firmware'})
        for name in report['payload_files']:
            self.assertFalse(any(token in name for token in ('fstab', 'ssh', 'machine-id', 'gdm3', '80-drivers', 'libinput')))
        self.assertIn('etc/wireplumber/wireplumber.conf.d/50-piano-audio.conf', report['payload_files'])
        self.assertNotIn('etc/wireplumber/wireplumber.conf.d/50-piano-camera.conf', report['payload_files'])
        row=report['payload_files']['etc/modules-load.d/piano-bluetooth.conf']
        self.assertEqual(row['source_component'],'Project SunUEFI')
        data=(bsp.BSP/'common/etc/modules-load.d/piano-bluetooth.conf').read_text()
        self.assertEqual([line for line in data.splitlines()if line and not line.startswith('#')],['uhid','hid_xiaomi'])

    def test_deepin_requires_explicit_suite(self):
        with self.assertRaisesRegex(ValueError, 'explicit'):
            bsp.build(self.args('inspect', '--target', 'deepin'))
        report = bsp.build(self.args('inspect', '--target', 'deepin', '--suite', 'user-selected-suite'))
        self.assertEqual(report['target']['suite'], 'user-selected-suite')
        self.assertFalse(report['dependency_resolution_tested'])

    def test_pen_calibration_uses_standard_firmware_paths_without_duplicate_tables(self):
        report = bsp.build(self.args())
        for panel in ('boe', 'csot'):
            name = f'usr/lib/firmware/novatek/piano-{panel}-pen.ini'
            row = report['payload_files'][name]
            self.assertEqual(row['type'], 'symlink')
            link = bsp.BSP / 'common' / name
            original = bsp.BSP / 'common/usr/share/piano/pen/calibration' / f'piano-{panel}.ini'
            self.assertEqual(link.resolve(), original.resolve())
            self.assertEqual(link.read_bytes(), original.read_bytes())

    def test_payload_tamper_refused_before_stage(self):
        copied = self.base / 'bsp'
        shutil.copytree(bsp.BSP, copied, symlinks=True)
        (copied / 'common/etc/modprobe.d/piano.conf').write_text('tampered\n')
        args = self.args('stage', '--output', str(self.base / 'stage'))
        with patch.object(bsp, 'BSP', copied):
            with self.assertRaisesRegex(ValueError, 'payload drift'):
                bsp.build(args)
        self.assertFalse(args.output.exists())

    def test_tracked_patches_apply_to_exact_public_sources(self):
        source = ROOT / 'tests/fixtures/bsp-upstream'
        manifest = json.loads((bsp.BSP / 'manifest.json').read_text())
        for row in manifest['files']:
            if not row.get('patch'):
                continue
            with self.subTest(path=row['path']):
                raw = (source / row['source']).read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), row['source_sha256'])
                path = self.base / row['source']
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
                subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', bsp.BSP / row['patch']], cwd=self.base, check=True, capture_output=True)
                layer = 'common' if row['group'] == 'common' else 'optional/' + row['group']
                self.assertEqual(path.read_bytes(), (bsp.BSP / layer / row['path']).read_bytes())
                if row.get('derived_from'):
                    derived = row['derived_from']
                    self.assertEqual(hashlib.sha256((ROOT / derived['path']).read_bytes()).hexdigest(), derived['sha256'])
        hifi = self.base / 'rootfs/overlay/usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf'
        self.assertIn(b"VA DMIC MUX0' DMIC2", hifi.read_bytes())
        self.assertIn(b'CaptureChannels 1', hifi.read_bytes())
        for position in ('Top Left', 'Top Right', 'Bottom Left', 'Bottom Right'):
            self.assertIn(f"{position} Amp Volume' 235".encode(), hifi.read_bytes())

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
    def test_pacman_sources_include_payload_and_native_backup_metadata(self):
        _, tree, report, rows = bsp.stage(
            self.args('stage', '--target', 'arch', '--output', str(self.base / 'stage')))
        work = self.base / 'makepkg'
        work.mkdir()
        configuration = [row['path'] for row, _ in rows if row['path'].startswith('etc/')]
        bsp.pacman_sources(work, tree, report, configuration)
        recipe = work / 'PKGBUILD'
        subprocess.run(['/bin/bash', '-n', recipe], check=True)
        metadata = subprocess.check_output(
            ['/bin/bash', '-c', 'source "$1"; printf "%s\\n" "${arch[@]}" "${backup[@]}"',
             'metadata', recipe], text=True).splitlines()
        self.assertEqual(metadata, ['any', *configuration])
        with tarfile.open(work / 'payload.tar') as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 0))
            member = archive.getmember('./usr/share/alsa/ucm2/conf.d/sm8750/Xiaomi Pad 8 Pro.conf')
            self.assertTrue(member.issym())
            self.assertEqual(member.linkname, '../../Qualcomm/sm8750/Xiaomi-Pad-8-Pro/Xiaomi-Pad-8-Pro.conf')
            hifi = archive.extractfile('./usr/share/alsa/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf').read()
            self.assertIn(b"VA_DEC0 Volume' 124", hifi)
        self.assertIn(hashlib.sha256((work / 'payload.tar').read_bytes()).hexdigest(),
                      recipe.read_text())

    def test_wrong_distro_or_missing_makepkg_does_not_stage(self):
        out = self.base / 'bundle'
        with self.assertRaisesRegex(ValueError, 'for Arch'):
            bsp.build(self.args('package', '--format', 'pacman', '--output', str(out)))
        with patch.object(bsp.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'Missing package backend: makepkg'):
                bsp.build(self.args('package', '--format', 'pacman', '--target', 'arch',
                                    '--output', str(out)))
        self.assertFalse(out.exists())

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

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable; no fake deb test')
    def test_real_touch_deb_contains_verified_firmware_only(self):
        out = self.base / 'deb'
        target = self.base / 'target'
        target.mkdir()
        source = bsp.TOUCH_MANIFEST.parent
        args = self.args('package', '--format', 'deb', '--touch-firmware', str(source),
                         '--output', str(out))
        with self.assertRaisesRegex(ValueError, 'Specify --against'):
            bsp.build(args)
        args.against = target
        self.assertFalse(out.exists())
        report = bsp.build(args)
        self.assertFalse(report['public_distribution'])
        artifact = out / next(iter(report['files']))
        self.assertEqual(artifact.name, 'piano-firmware-touch_0.1.0_all.deb')
        self.assertEqual(report['depends'], [])
        kib = subprocess.check_output(['dpkg-deb', '--field', artifact, 'Installed-Size'], text=True)
        with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(
                ['dpkg-deb', '--fsys-tarfile', artifact]))) as archive:
            members = archive.getmembers()
            files = {member.name for member in members if member.isfile()}
            self.assertEqual(int(kib), sum(1 if member.isdir() else (member.size + 1023) // 1024
                                           for member in members))
            self.assertEqual(files, {
                './usr/lib/firmware/novatek/novatek_nt36532_piano_fw_boe.bin',
                './usr/lib/firmware/novatek/novatek_nt36532_piano_fw_csot.bin',
                './usr/share/doc/piano-firmware-touch/bsp-manifest.json',
                './usr/share/doc/piano-firmware-touch/copyright',
            })
            for name, record in report['payload_files'].items():
                installed = archive.extractfile('./' + name).read()
                self.assertEqual(hashlib.sha256(installed).hexdigest(), record['sha256'])
            installed_report = archive.extractfile(
                './usr/share/doc/piano-firmware-touch/bsp-manifest.json').read().decode()
            self.assertNotIn(str(source), installed_report)

    def test_local_touch_firmware_rejects_tampering_and_publication_generation(self):
        source = self.base / 'source'
        shutil.copytree(bsp.TOUCH_MANIFEST.parent, source)
        args = self.args('stage', '--touch-firmware', str(source), '--output', str(self.base / 'stage'))
        with patch.dict(os.environ, {bsp.overlay_release.ENVIRONMENT: '10.1'}):
            with self.assertRaisesRegex(ValueError, 'cannot be built as a publication generation'):
                bsp.build(args)
        panel = source / 'novatek_nt36532_piano_fw_boe.bin'
        raw = panel.read_bytes()
        panel.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
        with self.assertRaisesRegex(ValueError, 'Touch firmware checksum mismatch'):
            bsp.build(args)
        self.assertFalse(args.output.exists())


if __name__ == '__main__':
    unittest.main()
