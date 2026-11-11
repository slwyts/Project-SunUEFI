"""Fixture-only policy/ZIP checks. No Android tool or partition is executed."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import build_android_module as mod


class AndroidModuleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.product = self.base / 'product'
        self.product.mkdir()
        fd = b'FIXTURE-FD'.ljust(0x300000, b'\0')
        shim = bytearray(64);shim[56:60] = b'ARM\x64'
        raw_app = b'FIXTURE-APP'
        app = struct.pack('<16sIIQ32s', b'SUNUEFI-APPv1\0'.ljust(16, b'\0'),
                          1, 64, len(raw_app), hashlib.sha256(raw_app).digest()) + raw_app
        kernel = gzip.compress(bytes(shim) + fd, mtime=0) + b'FIXTURE-DTB'
        header = bytearray(4096);header[:8] = b'ANDROID!'
        struct.pack_into('<II', header, 8, len(kernel), len(app))
        struct.pack_into('<I', header, 40, 4)
        image = bytes(header) + kernel.ljust(((len(kernel) + 4095) // 4096) * 4096, b'\0') + app
        self.payloads = {'fd.bin': fd, 'app.bin': app, 'shim.bin': bytes(shim)}
        parts = {'PianoUEFI-product.img': image, 'PianoUEFI-product.fd': fd, 'BootShim.bin': bytes(shim)}
        for name, data in parts.items():
            (self.product / name).write_bytes(data)
        self.write_json(self.product / 'manifest.json', {'android_header_version': 4,
            'files': {name: mod.record(data) for name, data in parts.items()}})
        self.selector = self.base / 'selector.bin'
        self.selector.write_bytes(b'\x1f\x20\x03\xd5' + bytes(12 + 128))  # NOP + metadata placeholder, never run.
        self.selector_meta = {'schema_version': 1, 'interface_version': 1,
            'selector_sha256': mod.digest(self.selector.read_bytes()), 'selector_bytes': 144,
            'selector_memory_bytes': 16384, 'metadata_offset': 16,
            'product_fd_sha256': mod.digest(fd), 'app_payload_sha256': mod.digest(app),
            'supported_boot_headers': [4], 'app_abi': 1, 'wrapper_version': 1,
            'entry_policy': 'explicit-request-only', 'request_bootarg': 'sunuefi.boot=uefi',
            'persistent_uefi_request': False}
        self.selector_manifest = self.base / 'selector.json'
        self.write_json(self.selector_manifest, self.selector_meta)
        elf = bytearray(124);elf[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HHIQQQ', elf, 16, 2, 183, 1, 0x1000, 64, 0)
        struct.pack_into('<HHH', elf, 52, 64, 56, 1)
        struct.pack_into('<IIQQQQQQ', elf, 64, 1, 5, 120, 0x1000, 0x1000, 4, 4, 4)
        elf[120:] = b'\xc0\x03\x5f\xd6'  # RET, structural fixture only; never run.
        self.tool = self.base / 'piano-boot-repack'
        self.tool.write_bytes(elf)
        evidence = self.base / 'fixture-evidence.txt'
        evidence.write_text('FIXTURE ONLY; not device evidence\n')
        self.proof = {'schema_version': 1, 'interface_version': 1,
            'executable': mod.record(bytes(elf)), 'commands': list(mod.COMMANDS),
            'request_policy': 'persistent-until-changed',
            'payload_sha256': {name: mod.digest(raw) for name, raw in
                {**self.payloads, 'selector.bin': self.selector.read_bytes()}.items()},
            'tested_boot_headers': [4], 'stock_kernel_bundled': False,
            'full_partition_backup': False, 'tests': {name: True for name in mod.TESTS},
            **{name: True for name in mod.DEVICE_PROOFS},
            'evidence': {name: {'path': evidence.name, **mod.record(evidence.read_bytes())} for name in mod.DEVICE_PROOFS}}
        self.native_manifest = self.base / 'native.json'
        self.write_json(self.native_manifest, self.proof)
        self.args = mod.parser().parse_args(['--product', str(self.product),
            '--selector', str(self.selector), '--selector-manifest', str(self.selector_manifest),
            '--native-tool', str(self.tool), '--native-manifest', str(self.native_manifest),
            '--output', str(self.base / 'fixture.zip')])

    def tearDown(self):
        self.temp.cleanup()

    def write_json(self, path, data):
        path.write_text(json.dumps(data))

    def shell(self, body):
        common = mod.ROOT / 'android/module/common.sh'
        return subprocess.run(['sh', '-c', '. "$1"; ui_print() { printf "%s\\n" "$*" >&2; }; ' + body,
                               'fixture', str(common)], text=True, capture_output=True)

    def test_product_esp_requires_nonempty_uki(self):
        esp = self.base / 'esp'
        entries = esp / 'loader/entries'
        entries.mkdir(parents=True)
        check = f'piano_esp_linux_present "{esp}"'
        self.assertNotEqual(self.shell(check).returncode, 0)
        boot = esp / 'EFI/Linux/sunuefi.efi'
        boot.parent.mkdir(parents=True)
        boot.touch()
        self.assertNotEqual(self.shell(check).returncode, 0)
        boot.write_bytes(b'EFI fixture')
        self.assertEqual(self.shell(check).returncode, 0)
        boot.unlink()
        self.assertNotEqual(self.shell(check).returncode, 0)

    def test_uki_path_stays_inside_esp_and_entry_contents_are_not_executed(self):
        esp = self.base / 'esp'
        entries = esp / 'loader/entries'
        entries.mkdir(parents=True)
        boot = esp / 'EFI/Linux/sunuefi.efi'
        boot.parent.mkdir(parents=True)
        (esp / 'payload').write_bytes(b'payload fixture')
        (self.base / 'outside').write_bytes(b'outside fixture')
        entry = entries / 'linux.conf'
        check = f'piano_esp_linux_present "{esp}"'
        boot.symlink_to(self.base / 'outside')
        self.assertNotEqual(self.shell(check).returncode, 0)
        boot.unlink()
        boot.write_bytes(b'EFI fixture')
        marker = self.base / 'must-not-run'
        entry.write_bytes((f'# comment\r\nlinux /payload\r\ninitrd /payload\r\n'
                           f'devicetree /payload\r\noptions $(touch "{marker}")').encode())
        self.assertEqual(self.shell(check).returncode, 0)
        self.assertFalse(marker.exists())

    def test_legacy_esp_does_not_claim_uki_is_installed(self):
        esp = self.base / 'esp'
        boot = esp / 'EFI/Piano/stable/boot.img'
        boot.parent.mkdir(parents=True)
        check = f'piano_esp_linux_present "{esp}"'
        boot.touch()
        self.assertNotEqual(self.shell(check).returncode, 0)
        boot.write_bytes(b'legacy boot image fixture')
        self.assertNotEqual(self.shell(check).returncode, 0)
        manager = esp / 'EFI/BOOT/BOOTAA64.EFI'
        manager.parent.mkdir(parents=True)
        manager.write_bytes(b'EFI fixture')
        self.assertNotEqual(self.shell(check).returncode, 0)

    def test_inspect_missing_native_is_readonly_and_no_zip(self):
        self.args.native_tool = self.args.native_manifest = None
        self.args.inspect = True
        report = mod.package(self.args)
        self.assertFalse(report['zip_ready'])
        self.assertFalse(report['device_passthrough_verified'])
        self.assertFalse(self.args.output.exists())

    def test_passthrough_false_blocks_zip_before_output(self):
        self.proof['device_passthrough_verified'] = False
        self.write_json(self.native_manifest, self.proof)
        with self.assertRaisesRegex(ValueError, 'device_passthrough_verified=false'):
            mod.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_request_and_stock_recovery_evidence_are_required(self):
        for flag in ('request_handling_verified', 'standard_recovery_preserved'):
            with self.subTest(flag=flag):
                self.proof[flag] = False
                self.write_json(self.native_manifest, self.proof)
                with self.assertRaisesRegex(ValueError, flag + '=false'):
                    mod.package(self.args)
                self.assertFalse(self.args.output.exists())
                self.proof[flag] = True

    def test_rec_shortcut_and_persistent_diagnostic_request_cannot_ship(self):
        for change in ({'entry_policy': 'recovery-enters-uefi'}, {'persistent_uefi_request': True}):
            with self.subTest(change=change):
                meta = {**self.selector_meta, **change}
                self.write_json(self.selector_manifest, meta)
                with self.assertRaises(ValueError):
                    mod.package(self.args)
                self.assertFalse(self.args.output.exists())

    def test_real_test_failure_and_changed_payload_block_zip(self):
        self.proof['tests']['ota_restore_refused'] = False
        self.write_json(self.native_manifest, self.proof)
        with self.assertRaisesRegex(ValueError, 'ota_restore_refused'):
            mod.package(self.args)
        self.proof['tests']['ota_restore_refused'] = True
        self.proof['payload_sha256']['app.bin'] = '0' * 64
        self.write_json(self.native_manifest, self.proof)
        with self.assertRaisesRegex(ValueError, 'different payload'):
            mod.package(self.args)

    def test_evidence_path_traversal_symlink_and_tamper_rejected(self):
        for path in ('../outside', '/absolute', 'link'):
            with self.subTest(path=path):
                if path == 'link':
                    (self.base / 'link').symlink_to(self.base / 'fixture-evidence.txt')
                self.proof['evidence']['device_passthrough_verified']['path'] = path
                self.write_json(self.native_manifest, self.proof)
                with self.assertRaises(ValueError):
                    mod.package(self.args)

    def test_elf_architecture_and_unbacked_entry_rejected(self):
        data = bytearray(self.tool.read_bytes())
        struct.pack_into('<H', data, 18, 62)
        with self.assertRaisesRegex(ValueError, 'ARM64'):
            mod.arm64_executable(data)
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 24, 0x2000)
        with self.assertRaisesRegex(ValueError, 'file-backed'):
            mod.arm64_executable(data)

    def test_product_tamper_incomplete_and_app_abi_rejected(self):
        (self.product / '.incomplete').touch()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            mod.package(self.args)
        (self.product / '.incomplete').unlink()
        image = self.product / 'PianoUEFI-product.img'
        image.write_bytes(image.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'hash/size'):
            mod.package(self.args)

    def test_selector_fd_compatibility_rejected(self):
        self.selector_meta['product_fd_sha256'] = '0' * 64
        self.write_json(self.selector_manifest, self.selector_meta)
        with self.assertRaisesRegex(ValueError, 'different FD/APP'):
            mod.package(self.args)

    def test_fixture_zip_has_payload_hashes_and_no_stock_images(self):
        report = mod.package(self.args)
        self.assertTrue(report['zip_ready'])
        with zipfile.ZipFile(self.args.output) as archive:
            names = set(archive.namelist())
            self.assertIn('service.sh', names)
            self.assertFalse(any(name.endswith('.img') for name in names))
            self.assertEqual(archive.read('payload/app.bin'), self.payloads['app.bin'])
            self.assertEqual(archive.read('payload/fd.bin'), self.payloads['fd.bin'])
            self.assertEqual(archive.read('payload/shim.bin'), self.payloads['shim.bin'])
            for row in archive.read('payload.sha256').decode().splitlines():
                sha, path = row.split('  ')
                self.assertEqual(sha, mod.digest(archive.read(path)))
            self.assertTrue(archive.getinfo('bin/piano-boot-repack').external_attr >> 16 & 0o111)
            policy = json.loads(archive.read('policy.json'))
            self.assertFalse(policy['ota_automatic'])
            self.assertFalse(policy['partition_execution_ready'])
            self.assertEqual(policy['entry_policy'], 'explicit-request-only')
            self.assertEqual(policy['request_policy'], 'persistent-until-changed')
            self.assertEqual(policy['selector'], {'memory_bytes': 16384, 'metadata_offset': 16})
            self.assertTrue(policy['standard_recovery_preserved'])
            status = json.loads(archive.read('webroot/status.json'))
            self.assertFalse(status['webui_bridge_verified'])

    def test_existing_output_is_preserved(self):
        self.args.output.write_bytes(b'EXISTING FIXTURE')
        with self.assertRaisesRegex(ValueError, 'new .zip'):
            mod.package(self.args)
        self.assertEqual(self.args.output.read_bytes(), b'EXISTING FIXTURE')

    def test_candidate_keeps_unverified_installation_blocked(self):
        self.proof['device_passthrough_verified'] = False
        self.write_json(self.native_manifest, self.proof)
        self.args.candidate = True
        report = mod.package(self.args)
        self.assertFalse(report['zip_ready'])
        self.assertFalse(report['install_ready'])
        self.assertEqual(report['artifact_kind'], 'validation-candidate')
        with zipfile.ZipFile(self.args.output) as archive:
            policy = json.loads(archive.read('policy.json'))
            self.assertFalse(policy['zip_ready'])
            self.assertIn(b'PIANO_DEVICE_PASSTHROUGH_VERIFIED=false', archive.read('module-policy.sh'))
            self.assertIn('引导检查尚未通过', archive.read('module.prop').decode())
        result = self.shell('PIANO_POLICY_SCHEMA=1; PIANO_INTERFACE_VERSION=1; '
            'PIANO_DEVICE_PASSTHROUGH_VERIFIED=false; PIANO_REQUEST_HANDLING_VERIFIED=true; '
            'PIANO_STANDARD_RECOVERY_PRESERVED=true; piano_check_payload')
        self.assertNotEqual(result.returncode, 0)

    def test_manager_metadata_is_excluded_from_installed_checksums(self):
        mod.package(self.args)
        with zipfile.ZipFile(self.args.output) as archive:
            hashes = archive.read('payload.sha256').decode()
            self.assertNotIn('META-INF/', hashes)
            self.assertNotIn('  module.prop\n', hashes)
            self.assertNotIn('  customize.sh\n', hashes)
            self.assertEqual(archive.read('META-INF/com/google/android/updater-script'), b'#MAGISK\n')
            self.assertTrue(archive.getinfo('META-INF/com/google/android/update-binary').external_attr >> 16 & 0o111)

    def test_partition_pair_skips_choice_incomplete_pair_fails(self):
        folder = self.base / 'by-name';folder.mkdir()
        self.assertEqual(self.shell('piano_partition_state "' + str(folder) + '"').stdout.strip(), 'absent')
        (folder / 'sunuefi_esp').touch()
        self.assertNotEqual(self.shell('piano_partition_state "' + str(folder) + '"').returncode, 0)
        (folder / 'sunuefi_root').touch()
        self.assertEqual(self.shell('piano_partition_state "' + str(folder) + '"').stdout.strip(), 'existing')

    def test_action_without_tool_is_readonly_status_and_request_refused(self):
        script = mod.ROOT / 'android/module/action.sh'
        env = {**os.environ, 'PIANO_LANG': 'zh'}
        action = subprocess.run(['sh', str(script)], env=env, text=True, capture_output=True)
        self.assertEqual(action.returncode, 0)
        self.assertIn('打开 WebUI', action.stdout)
        self.assertNotIn('{', action.stdout)
        status = subprocess.run(['sh', str(script), 'status'], env=env, text=True, capture_output=True)
        self.assertEqual(status.returncode, 0)
        self.assertFalse(json.loads(status.stdout)['request_handling_verified'])
        request = subprocess.run(['sh', str(script), 'request', 'uefi', '--confirm'], text=True, capture_output=True)
        self.assertNotEqual(request.returncode, 0)
        bad = subprocess.run(['sh', str(script), 'request', 'uefi;touch /tmp/not-a-command', '--confirm'], capture_output=True)
        self.assertEqual(bad.returncode, 2)

    @unittest.skipUnless(shutil.which('node'), 'Node needed for optional WebUI model checks')
    def test_webui_validates_actions_and_native_callback(self):
        model = mod.ROOT / 'android/module/webroot/main.js'
        body = """const assert=require('node:assert/strict'), m=require(process.argv[1]);
          async function main() {
            global.window={};
            await assert.rejects(m.nativeAction('switch','linux'));
            let calls=0;
            window.ksu={exec(command,options,callback) {
              calls++;
              assert.equal(command,'/system/bin/sh /data/adb/modules/piano_sunuefi/action.sh quick-status');
              assert.equal(options,'{}');
              window[callback](0,JSON.stringify({wrapped:true,bootloader_confirm_required:true}),'');
              assert.equal(window[callback],undefined);
            }};
            assert.deepEqual(await m.nativeAction('quick-status'),{wrapped:true,bootloader_confirm_required:true});
            for (const args of [['switch','uefi;bad'], ['storage-execute','bad;id'],
                ['storage-plan','create','$(id)'], ['storage-select','esp','3b6964','-1'],
                ['storage-plan','flash'], ['storage-plan','flash','none'],
                ['storage-plan','flash','esp;id'], ['storage-plan','flash','32'],
                ['confirm-bootloader']]) await assert.rejects(m.nativeAction(...args));
            assert.equal(calls,1);
            const planned=[];
            window.ksu.exec=(command,_options,callback)=>{
              planned.push(command);
              window[callback](0,JSON.stringify({plan_id:'test-plan'}),'');
            };
            for (const scope of ['esp','root','both']) {
              assert.deepEqual(await m.nativeAction('storage-plan','flash',scope),{plan_id:'test-plan'});
            }
            assert.deepEqual(planned,['esp','root','both'].map(scope=>
              '/system/bin/sh /data/adb/modules/piano_sunuefi/action.sh storage-plan flash '+scope));
            window.ksu.exec=(_command,_options,callback)=>window[callback](1,'','已按音量上取消，尚未写入。');
            await assert.rejects(m.nativeAction('reinstall'),/音量上取消/);
          }
          main().catch(error=>{console.error(error);process.exitCode=1;});
        """
        result = subprocess.run(['node', '-e', body, str(model)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
