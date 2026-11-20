import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build.check_patch_series import patch_id
from build import overlay_packages
from package.archive import pacman_info
import package_bsp as bsp
from stage_piano_firmware import packaged_components

TOPOLOGY_TOOLS = all(shutil.which(tool) for tool in ('m4', 'alsatplg'))
TOPOLOGY_SOURCES = all((ROOT / 'upstream' / name / '.git').exists() for name in bsp.TOPOLOGY_SOURCES)
DMA_HEAP_RULE = 'usr/lib/udev/rules.d/70-piano-camera.rules'
FPRINTD_DROP_IN = 'usr/lib/systemd/system/fprintd.service.d/10-piano-goodix.conf'


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
        self.assertIn(DMA_HEAP_RULE, report['payload_files'])
        self.assertIn(FPRINTD_DROP_IN, report['payload_files'])
        self.assertNotIn(bsp.OLD_FPRINTD_DROP_IN, report['payload_files'])
        self.assertNotIn('etc/udev/rules.d/70-piano-camera.rules', report['payload_files'])
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

    def test_pen_service_executes_a_file_in_the_same_package_without_a_device(self):
        out = self.base / 'stage'
        bsp.build(self.args('stage', '--output', str(out)))
        tree = out / 'payload'
        unit = tree / 'usr/lib/systemd/system/piano-touch.service'
        command = next(line.removeprefix('ExecStart=').split()
                       for line in unit.read_text().splitlines() if line.startswith('ExecStart='))
        self.assertEqual(command[1:], ['--follow'])
        executable = tree / command[0].lstrip('/')
        self.assertTrue(executable.is_file())
        self.assertEqual(executable.stat().st_mode & 0o777, 0o755)
        self.assertIn('--follow', subprocess.check_output([executable, '--help'], text=True))

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
                    self.assertEqual(patch_id(ROOT / derived['path'], verbatim=True),
                                     (derived['patch_id'],), derived['scope'])
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
            ['/bin/bash', '-c', 'source "$1"; printf "%s\\n" "${arch[@]}" "${backup[@]}" "${depends[@]}"',
             'metadata', recipe], text=True).splitlines()
        self.assertEqual(metadata, ['any', *configuration, *report['depends']])
        self.assertTrue({'python', 'python-gobject', 'bluez'} <= set(metadata))
        self.assertNotIn(DMA_HEAP_RULE, metadata)
        self.assertNotIn(FPRINTD_DROP_IN, metadata)
        with tarfile.open(work / 'payload.tar') as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 0))
            self.assertEqual(archive.getmember('./' + DMA_HEAP_RULE).mode, 0o644)
            self.assertEqual(archive.getmember('./' + FPRINTD_DROP_IN).mode, 0o644)
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
            self.assertIn('./usr/lib/piano/pen-control.py', archive.getnames())
            self.assertEqual(archive.getmember('./' + DMA_HEAP_RULE).mode, 0o644)
            drop_in = archive.extractfile('./' + FPRINTD_DROP_IN).read()
            self.assertEqual(set(drop_in.splitlines()), {
                b'[Service]', b'StateDirectory=piano-goodix', b'StateDirectoryMode=0700',
                b'DeviceAllow=char-uio rw', b'DeviceAllow=char-tee rw', b'DeviceAllow=char-bsg rw',
            })
            self.assertNotIn('./' + bsp.OLD_FPRINTD_DROP_IN, archive.getnames())
        control = self.base / 'control'
        subprocess.run(['dpkg-deb', '--control', artifact, control], check=True)
        guard = (control / 'preinst').read_text()
        self.assertIn('dpkg-query', guard)
        self.assertNotIn('systemctl', guard)
        fields = (control / 'control').read_text()
        self.assertNotIn('/' + DMA_HEAP_RULE, (control / 'conffiles').read_text().splitlines())
        self.assertNotIn('/' + FPRINTD_DROP_IN, (control / 'conffiles').read_text().splitlines())
        depends = next(line.removeprefix('Depends: ').split(', ')
                       for line in fields.splitlines() if line.startswith('Depends: '))
        self.assertTrue({'python3', 'python3-gi', 'gir1.2-glib-2.0', 'bluez'} <= set(depends))
        self.assertNotIn('Replaces:', fields)

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

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable; no fake deb test')
    def test_real_display_deb_contains_both_panel_calibrations(self):
        source = bsp.DISPLAY_MANIFEST.parent
        out = self.base / 'deb'
        args = self.args('package', '--format', 'deb', '--display-firmware', str(source),
                         '--output', str(out))
        report = bsp.build(args)
        self.assertEqual(report['component'], 'display-firmware')
        self.assertFalse(report['public_distribution'])
        self.assertFalse(report['conflict_check']['performed'])
        self.assertEqual(report['depends'], [])
        artifact = out / next(iter(report['files']))
        self.assertEqual(artifact.name, 'piano-firmware-display_0.1.0_all.deb')
        manifest = json.loads(bsp.DISPLAY_MANIFEST.read_text())
        firmware = 'usr/lib/firmware/qcom/sm8750/'
        docs = './usr/share/doc/piano-firmware-display/'
        with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(
                ['dpkg-deb', '--fsys-tarfile', artifact]))) as archive:
            self.assertEqual({member.name for member in archive if member.isfile()}, {
                './' + firmware + 'piano-boe-display.bin', './' + firmware + 'piano-csot-display.bin',
                docs + 'bsp-manifest.json', docs + 'copyright',
            })
            for name, record in manifest['files'].items():
                entry = archive.getmember('./' + firmware + name)
                self.assertEqual((entry.mode, entry.uid, entry.gid), (0o644, 0, 0))
                data = archive.extractfile(entry).read()
                self.assertEqual((len(data), hashlib.sha256(data).hexdigest()),
                                 (record['bytes'], record['sha256']))
                self.assertEqual(report['payload_files'][firmware + name]['source_sha256'],
                                 record['source_sha256'])
            provenance = archive.extractfile(docs + 'bsp-manifest.json').read().decode()
            self.assertNotIn(str(source), provenance)
            self.assertEqual(json.loads(provenance)['source']['rom'], manifest['rom'])
            self.assertNotIn('conflict_check', json.loads(provenance))
        control = self.base / 'control'
        subprocess.run(['dpkg-deb', '--control', artifact, control], check=True)
        self.assertFalse((control / 'conffiles').exists())
        self.assertFalse((control / 'postinst').exists())
        self.assertNotIn('Replaces:', (control / 'control').read_text())

    def test_display_migration_accepts_only_exact_unowned_raw_files(self):
        source = bsp.DISPLAY_MANIFEST.parent
        _, rows = bsp.local_firmware_payload(source, bsp.DISPLAY_MANIFEST, 'display firmware')
        name = rows[0][0]['path']
        root = self.base / 'guest'
        path = root / name
        path.parent.mkdir(parents=True)
        path.write_bytes(rows[0][1])
        path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'unowned-existing-file'):
            bsp.conflict_check(root, rows, bsp.DISPLAY_PACKAGE)
        self.assertEqual(bsp.conflict_check(root, rows, bsp.DISPLAY_PACKAGE,
                                            adopt_identical=True)['adopted'], [name])
        compressed = path.with_name(path.name + '.zst')
        compressed.write_bytes(b'compressed alias')
        with self.assertRaisesRegex(ValueError, r'\.zst'):
            bsp.conflict_check(root, rows, bsp.DISPLAY_PACKAGE, adopt_identical=True)
        compressed.unlink()
        path.write_bytes(b'not the fixed calibration')
        with self.assertRaisesRegex(ValueError, 'unowned-existing-file'):
            bsp.conflict_check(root, rows, bsp.DISPLAY_PACKAGE, adopt_identical=True)

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable; no fake deb test')
    def test_keyboard_deb_contains_pinned_firmware_and_uses_native_initramfs_trigger(self):
        manifest = bsp.KEYBOARD_MANIFEST
        source = json.loads(manifest.read_text())
        out = self.base / 'deb'
        with patch.dict(os.environ, {bsp.overlay_release.ENVIRONMENT: '10.1'}):
            report = bsp.build(self.args('package', '--format', 'deb', '--keyboard-firmware',
                                         str(manifest.parent), '--output', str(out)))
        relative = 'usr/lib/firmware/nanosic/MCU_Upgrade.bin'
        artifact, metadata = overlay_packages.bsp_package(out, 'debian', component='keyboard-firmware')
        self.assertEqual(artifact.name, f"{bsp.KEYBOARD_PACKAGE}_{source['rom'][2:]}+sunuefi.10.1_all.deb")
        self.assertTrue(report['public_distribution'])
        self.assertEqual(report['depends'], [])
        self.assertEqual(set(metadata['firmware']), {relative})
        with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(
                ['dpkg-deb', '--fsys-tarfile', artifact]))) as archive:
            files = [member for member in archive if member.isfile()
                     and member.name.startswith('./usr/lib/firmware/')]
            self.assertEqual([member.name for member in files], ['./' + relative])
            entry = files[0]
            self.assertEqual((entry.mode, entry.uid, entry.gid), (0o644, 0, 0))
            expected = source['files']['MCU_Upgrade.bin']
            data = archive.extractfile(entry).read()
            self.assertEqual((len(data), hashlib.sha256(data).hexdigest()),
                             (expected['bytes'], expected['sha256']))
        control = self.base / 'control'
        subprocess.run(['dpkg-deb', '--control', artifact, control], check=True)
        self.assertEqual((control / 'triggers').read_text(), 'activate-noawait update-initramfs\n')
        self.assertFalse((control / 'conffiles').exists())
        self.assertEqual(packaged_components([(PurePosixPath('nanosic'), manifest, source)]),
                         {'piano-keyboard': 'keyboard-firmware'})

    @unittest.skipUnless(shutil.which('dpkg-query'), 'dpkg-query unavailable')
    def test_keyboard_preinst_checks_merged_usr_ownership(self):
        manifest = bsp.KEYBOARD_MANIFEST
        _, rows = bsp.local_firmware_payload(manifest.parent, manifest, 'keyboard firmware')
        (row, data), = rows
        relative = row['path']
        root = self.base / 'guest'
        firmware = root / relative
        firmware.parent.mkdir(parents=True)
        firmware.write_bytes(data)
        firmware.chmod(0o644)
        (root / 'lib').symlink_to('usr/lib')
        database = root / 'var/lib/dpkg'
        info = database / 'info'
        info.mkdir(parents=True)
        status = database / 'status'
        status.touch()
        script = bsp.preinst([relative], bsp.KEYBOARD_PACKAGE, adopt={relative: row})
        environment = {**os.environ, 'DPKG_ROOT': str(root), 'DPKG_ADMINDIR': str(database)}

        def preinst():
            return subprocess.run(['/bin/sh', '-c', script], env=environment,
                                  capture_output=True, text=True)

        def record_alias_owner(package):
            status.write_text(
                f'Package: {package}\nStatus: install ok installed\n'
                'Priority: optional\nSection: kernel\nArchitecture: all\nVersion: 1\n'
                'Maintainer: Test <test@example.invalid>\nDescription: Alias owner\n')
            ownership = info / (package + '.list')
            ownership.write_text('/' + relative.removeprefix('usr/') + '\n')
            return ownership

        result = preinst()
        self.assertEqual(result.returncode, 0, result.stderr)
        foreign = record_alias_owner('other-keyboard-firmware')
        result = preinst()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('other-keyboard-firmware', result.stderr)
        foreign.unlink()
        record_alias_owner(bsp.KEYBOARD_PACKAGE)
        firmware.write_bytes(b'updated bytes from the installed package')
        result = preinst()
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('makepkg', 'bsdtar')),
                         'pacman packaging tools required')
    def test_keyboard_pacman_uses_native_backup_metadata_for_legacy_files(self):
        out = self.base / 'pacman'
        bsp.build(self.args('package', '--target', 'arch', '--format', 'pacman', '--keyboard-firmware',
                            str(bsp.KEYBOARD_MANIFEST.parent), '--output', str(out)))
        archive, metadata = overlay_packages.bsp_package(out, 'pacman', component='keyboard-firmware')
        relative = 'usr/lib/firmware/nanosic/MCU_Upgrade.bin'
        self.assertEqual(pacman_info(archive).get('backup'), [relative])
        self.assertEqual(set(metadata['firmware']), {relative})

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb unavailable; no fake deb test')
    def test_fingerprint_deb_preserves_all_raw_firmware_at_the_loader_paths(self):
        manifest = bsp.FINGERPRINT_MANIFEST
        out = self.base / 'deb'
        args = self.args('package', '--format', 'deb', '--fingerprint-firmware',
                         str(manifest.parent), '--output', str(out))
        report = bsp.build(args)
        self.assertEqual(report['component'], 'fingerprint-firmware')
        self.assertFalse(report['public_distribution'])
        self.assertEqual(report['depends'], [])
        artifact = out / next(iter(report['files']))
        lock = json.loads(manifest.read_text())
        version = lock['rom'].removeprefix('OS')
        self.assertEqual(artifact.name, f'piano-firmware-fingerprint_{version}_all.deb')
        pinned = lock['files']
        paths = {'./usr/lib/firmware/' + name: record for name, record in pinned.items()}
        self.assertEqual(set(pinned), {'gfpia.elf', 'piano-goodix-cap-s.bin',
                                      'piano-goodix-cap-t.bin', 'piano-goodix-dubai-u4.bin'})
        data = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', artifact])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for path, expected in paths.items():
                entry = archive.getmember(path)
                self.assertTrue(entry.isfile())
                self.assertEqual((entry.mode, entry.uid, entry.gid), (0o644, 0, 0))
                installed = archive.extractfile(entry).read()
                self.assertEqual((len(installed), hashlib.sha256(installed).hexdigest()),
                                 (expected['bytes'], expected['sha256']))
            self.assertEqual({member.name for member in archive
                              if not member.isdir() and member.name.startswith('./usr/lib/firmware/')},
                             set(paths))

    def test_packaged_fingerprint_cannot_be_shadowed_by_a_direct_copy(self):
        manifest = bsp.FINGERPRINT_MANIFEST
        pinned = json.loads(manifest.read_text())
        selected = [(PurePosixPath('.'), manifest, pinned)]
        self.assertEqual(packaged_components(selected),
                         {pinned['component']: 'fingerprint-firmware'})
        with self.assertRaisesRegex(ValueError, 'differs from the fixed source'):
            packaged_components([(PurePosixPath('.'), manifest, {**pinned, 'rom': 'modified'})])
        with self.assertRaisesRegex(ValueError, 'overlaps firmware owned by a native package'):
            packaged_components([*selected, (PurePosixPath('.'), manifest,
                                              {**pinned, 'component': 'other'})])

    def test_power_profiles_adopt_only_identical_unowned_defaults(self):
        root = self.base / 'guest'
        row, data = bsp.power_profiles_payload()[0]
        path = root / row['path']
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        path.chmod(0o644)

        def check():
            return bsp.conflict_check(root, [(row, data)], bsp.POWER_PROFILES_PACKAGE,
                                      adopt_identical=True)

        self.assertEqual(check()['adopted'], [row['path']])
        path.write_bytes(data + b'\n# Administrator default\n')
        with self.assertRaisesRegex(ValueError, 'unowned-existing-file'):
            check()
        path.write_bytes(data)
        info = root / 'var/lib/dpkg/info'
        info.mkdir(parents=True)
        (info / 'other.list').write_text('/' + row['path'] + '\n')
        with self.assertRaisesRegex(ValueError, 'other'):
            check()

    def test_local_firmware_rejects_tampering_and_publication_generation(self):
        for component, manifest in (('touch', bsp.TOUCH_MANIFEST), ('display', bsp.DISPLAY_MANIFEST),
                                    ('fingerprint', bsp.FINGERPRINT_MANIFEST)):
            with self.subTest(component=component):
                source = self.base / component
                shutil.copytree(manifest.parent, source)
                args = self.args('stage', f'--{component}-firmware', str(source),
                                 '--output', str(self.base / 'stage'))
                with patch.dict(os.environ, {bsp.overlay_release.ENVIRONMENT: '10.1'}):
                    with self.assertRaisesRegex(ValueError, 'cannot be built as a publication generation'):
                        bsp.build(args)
                panel = source / next(iter(json.loads(manifest.read_text())['files']))
                raw = panel.read_bytes()
                panel.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
                with self.assertRaisesRegex(ValueError, f'{component.capitalize()} firmware checksum mismatch'):
                    bsp.build(args)
                self.assertFalse(args.output.exists())

    @unittest.skipUnless(TOPOLOGY_TOOLS and TOPOLOGY_SOURCES and shutil.which('dpkg-deb'),
                         'pinned AudioReach sources and Debian build tools required')
    def test_real_audio_topology_deb_contains_only_pinned_topology_and_license(self):
        out = self.base / 'deb'
        args = self.args('package', '--audio-topology', '--format', 'deb', '--output', str(out))
        with patch.dict(os.environ, {bsp.overlay_release.ENVIRONMENT: '10.1'}):
            report = bsp.build(args)
        artifact = out / next(iter(report['files']))
        self.assertEqual(report['component'], 'audio-topology')
        self.assertTrue(report['public_distribution'])
        self.assertFalse(report['conflict_check']['performed'])
        self.assertEqual(artifact.name, 'piano-audio-topology_0.1.0+sunuefi.10.1_all.deb')
        self.assertEqual(report['depends'], [])
        name = 'usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
        self.assertEqual(set(report['payload_files']), {name})
        docs = './usr/share/doc/piano-audio-topology/'
        with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(
                ['dpkg-deb', '--fsys-tarfile', artifact]))) as archive:
            self.assertEqual({member.name for member in archive if member.isfile()},
                             {'./' + name, docs + 'bsp-manifest.json', docs + 'copyright'})
            topology = archive.getmember('./' + name)
            self.assertEqual((topology.uid, topology.gid, topology.mode), (0, 0, 0o644))
            data = archive.extractfile(topology).read()
            policy = json.loads((ROOT / 'config/release.json').read_text())['audio_topology']
            self.assertEqual(hashlib.sha256(data).hexdigest(), policy['sha256'])
            license_text = archive.extractfile(docs + 'copyright').read()
            self.assertIn(b'BSD-3-Clause', license_text)
            self.assertIn(b'Copyright 2021-2024 Linaro Ltd.', license_text)
        control = self.base / 'control'
        subprocess.run(['dpkg-deb', '--control', artifact, control], check=True)
        self.assertFalse((control / 'conffiles').exists())
        self.assertNotIn('Replaces:', (control / 'control').read_text())

    @unittest.skipUnless(TOPOLOGY_TOOLS and TOPOLOGY_SOURCES,
                         'pinned AudioReach sources and build tools required')
    def test_topology_preflight_distinguishes_merged_own_file_from_aliases(self):
        guest = self.base / 'guest'
        _, rows = bsp.topology_payload()
        name = rows[0][0]['path']
        original = guest / name
        original.parent.mkdir(parents=True)
        original.write_bytes(rows[0][1])
        (guest / 'lib').symlink_to('/usr/lib')
        info = guest / 'var/lib/dpkg/info'
        info.mkdir(parents=True)
        ownership = info / 'piano-audio-topology.list'
        ownership.write_text('/' + name.removeprefix('usr/') + '\n')
        self.assertTrue(bsp.conflict_check(guest, rows, bsp.TOPOLOGY_PACKAGE)['performed'])
        ownership.unlink()
        with self.assertRaisesRegex(ValueError, 'unowned-existing-file'):
            bsp.conflict_check(guest, rows, bsp.TOPOLOGY_PACKAGE)
        original.unlink()
        (guest / 'lib').unlink()
        for suffix in ('.xz', '.zst'):
            with self.subTest(suffix=suffix):
                alt = name.removeprefix('usr/') + suffix
                compressed = guest / alt
                compressed.parent.mkdir(parents=True, exist_ok=True)
                compressed.write_bytes(b'an existing firmware file')
                db = guest / 'usr/lib/holo/pacmandb/local/linux-firmware-qcom-1-1'
                db.mkdir(parents=True, exist_ok=True)
                (db / 'files').write_text(f'%FILES%\n{alt}\n')
                with self.assertRaisesRegex(ValueError, 'linux-firmware-qcom'):
                    bsp.conflict_check(guest, rows, bsp.TOPOLOGY_PACKAGE)
                self.assertEqual(compressed.read_bytes(), b'an existing firmware file')
                compressed.unlink()


if __name__ == '__main__':
    unittest.main()
