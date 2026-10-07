"""Actual sealed kernels/DTB/root provenance plus bounded CPIO corruption tests."""
from pathlib import Path
import hashlib
import json
import shutil
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import assemble_piano_linux as assemble
from build_piano_ram_bootstrap import write_newc

DT_DIRS = ('piano-full-dtb-fd6266-impact-fixed', 'piano-linux-owned-dma',
           'piano-linux-managed-clocks-v1', 'piano-linux-managed-dsp-pcie-v1')
LIB = ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1'


class AssemblePianoLinuxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kernels = {family: assemble.kernel(path, family) for family, path in assemble.DEFAULT_KERNELS.items()}

    def test_actual_sealed_kernels_are_full_efi_and_debug_limit_disclosed(self):
        self.assertEqual(len(self.kernels), 2)
        for family, kernel in self.kernels.items():
            self.assertEqual(kernel['source_commit'], assemble.COMMITS[family])
            self.assertTrue(kernel['image']['efi_stub'])
            self.assertEqual(len(kernel['required_hardware_modules']), 18)
            self.assertTrue(kernel['config_cmdline_force'])
            self.assertFalse(kernel['external_debug_parameters_effective'])
            self.assertNotIn('userdata', kernel['command_line'])

    def test_actual_full_dtb_chain_and_stage_mutation_rejected(self):
        manifests = [ROOT / 'private/analysis' / name / 'manifest.json' for name in DT_DIRS]
        dtb = manifests[-1].parent / 'Piano-full-linux-managed-dsp-pcie.dtb'
        result = assemble.device_tree(dtb, manifests, LIB)
        self.assertEqual(result['sha256'], 'c2cb041e2b286713c225af7bf0e3a5f7eec8926db168149984823c25ad3f38c4')
        self.assertFalse(result['memory_ownership_authorized'])
        with tempfile.TemporaryDirectory(prefix='assemble-dtb-') as directory:
            folder = Path(directory)
            original = manifests[-1]
            shutil.copyfile(dtb, folder / dtb.name)
            value = json.loads(original.read_text());value['changes'] = []
            broken = folder / 'manifest.json';broken.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError, 'property changes'):
                assemble.device_tree(dtb, manifests[:-1] + [broken], LIB)

    def test_obsolete_root_missing_final_releases_is_rejected_before_big_read(self):
        root = ROOT / 'build/distros/debian13-piano-full'
        old = ROOT / 'artifacts/linux-full-rootfs/gnome-20261006-runtime-complete'
        before = assemble.sha_file(old / 'manifest.json')
        with self.assertRaisesRegex(ValueError, 'Required regular root member missing'):
            assemble.payload(old, root, self.kernels)
        self.assertEqual(before, assemble.sha_file(old / 'manifest.json'))

    def test_small_actual_newc_members_and_corrupt_runtime_rejected(self):
        payload = b'compressed-root-fixture';entry = b'#!/bin/sh\nexit 1\n'
        digest = lambda data: hashlib.sha256(data).hexdigest()
        root = {'archive_sha256': digest(payload), 'archive_bytes': len(payload), 'regular_page_budget_bytes': 4096}
        with tempfile.TemporaryDirectory(prefix='assemble-cpio-') as directory:
            folder = Path(directory);archive = folder / 'initramfs.cpio'
            def make(busybox):
                members = {'bin/busybox': busybox, 'pianoinit': entry, 'rootfs.tar.gz': payload,
                           'rootfs.sha256': (root['archive_sha256'] + '  rootfs.tar.gz\n').encode(),
                           'rootfs.page-budget': b'4096\n'}
                with archive.open('wb') as stream:
                    for inode, (name, data) in enumerate(members.items(), 1):
                        write_newc(stream, name, stat.S_IFREG | 0o755, inode, data=data)
                    write_newc(stream, 'TRAILER!!!', 0, 9, data=b'')
                    stream.write(bytes(-stream.tell() % 512))
                manifest = {'status': 'HOST_BUILT_GNU_ROOT_BOOTSTRAP_NOT_BOOT_VERIFIED',
                            'root_payload_sha256': root['archive_sha256'], 'root_limit_bytes': assemble.LIMIT,
                            'root_extraction_passes': 1, 'expanded_root_page_budget_bytes': 4096,
                            'initramfs_bytes': archive.stat().st_size, 'initramfs_sha256': assemble.sha_file(archive),
                            'gnu_tar_runtime': {}, 'entry_sha256': digest(entry)}
                (folder / 'manifest.json').write_text(json.dumps(manifest))
            # The known binary is small; use its actual verified bytes.
            busybox = (ROOT / 'build/linux-ram/busybox').read_bytes()
            make(busybox)
            result = assemble.bootstrap(folder, root)
            self.assertTrue(result['embedded_root_payload_verified'])
            make(b'unknown busybox')
            with self.assertRaisesRegex(ValueError, 'runtime/payload member differs'):
                assemble.bootstrap(folder, root)

    def test_actual_small_efi_tree_copy_is_disabled_and_never_overwrites(self):
        with tempfile.TemporaryDirectory(prefix='assemble-tree-') as directory:
            folder = Path(directory)
            source = folder / 'image';source.write_bytes(b'small-copy-fixture')
            files = {'/EFI/Piano/stable/Image.efi': {'source': str(source), 'bytes': source.stat().st_size,
                                                    'sha256': assemble.sha_file(source)}}
            report = {'entries': {'stable': {'enabled': False}}}
            output = folder / 'artifacts/linux-assembled/test'
            with patch.object(assemble, 'ROOT', folder):
                assemble.materialize(files, report, output)
                self.assertEqual((output / 'EFI/Piano/stable/Image.efi').read_bytes(), source.read_bytes())
                self.assertFalse(json.loads((output / 'manifest.json').read_text())['entries']['stable']['enabled'])
                with self.assertRaisesRegex(ValueError, 'fresh'):
                    assemble.materialize(files, report, output)
                with self.assertRaisesRegex(ValueError, 'cannot enable'):
                    assemble.materialize(files, {'entries': {'stable': {'enabled': True}}}, output.parent / 'other')

    def test_tree_duplicate_and_escaping_path_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            assemble.safe_name('../outside')
        rows = [{'name': '.', 'mode': stat.S_IFDIR | 0o755}, {'name': '.', 'mode': stat.S_IFDIR | 0o755}]
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            assemble.tree_records({'records': rows, 'entries': 2, 'tree_sha256': '0' * 64})


if __name__ == '__main__':
    unittest.main()
