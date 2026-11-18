"""Shared CI generations retain hashes, build checks and one writer per cache."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('shared_products', ROOT / 'tools/build/shared_products.py')
shared = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shared)
import ci_build_targets


class SharedProductsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='shared-products-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def git(self, *args, epoch=None):
        env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
        if epoch is not None:
            env.update(GIT_AUTHOR_DATE=f'@{epoch} +0000', GIT_COMMITTER_DATE=f'@{epoch} +0000')
        return subprocess.check_output(['git', '-C', self.root, *args], env=env, text=True).strip()

    def test_keys_bind_actual_inputs_environment_and_local_edits(self):
        self.git('init', '-q')
        path = self.root / 'tools/recipe.py'
        path.parent.mkdir()
        path.write_text('first\n')
        self.git('add', '.')
        with patch.object(shared, 'FIRMWARE_INPUTS', ('tools',)), \
                patch.object(shared, 'environment_identity', return_value={'container_image': 'sha256:a'}):
            initial = shared.identity('firmware', self.root)[1]
            path.write_text('second\n')
            self.assertNotEqual(shared.identity('firmware', self.root)[1], initial)
            initial = shared.identity('firmware', self.root)[1]
            (path.parent / 'local.py').write_text('untracked build helper\n')
            self.assertNotEqual(shared.identity('firmware', self.root)[1], initial)
            initial = shared.identity('firmware', self.root)[1]
            with patch.object(shared, 'environment_identity', return_value={'container_image': 'sha256:b'}):
                self.assertNotEqual(shared.identity('firmware', self.root)[1], initial)

    def test_verified_generation_reused_without_running_firmware_build(self):
        output = self.root / 'artifacts/product/payload'
        output.parent.mkdir(parents=True)
        output.write_bytes(b'actual output bytes')
        identity = {'schema_version': 1, 'kind': 'firmware', 'inputs': {'source': 'hash'}}
        record = {'identity': identity, 'outputs': shared.inventory([output], self.root),
                  'verification': {'verified': True}}
        marker = self.root / 'build/shared-products/firmware.json'
        shared.atomic_json(marker, record)
        self.assertEqual(marker.stat().st_mode & 0o777, 0o644)
        with patch.object(shared, 'identity', return_value=(identity, 'key')), \
                patch.object(shared, 'output_paths', return_value=[output]), \
                patch.object(shared, 'verify_firmware', return_value={'verified': True}) as verify, \
                patch.object(shared.subprocess, 'run') as command:
            shared.run('firmware', verify_only=True, root=self.root)
            verify.assert_called_once_with(self.root)
            command.assert_not_called()
            output.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'stale or modified'):
                shared.run('firmware', verify_only=True, root=self.root)
            command.assert_not_called()

    def test_firmware_key_ignores_host_checks_and_rootfs_assembly_but_binds_firmware_tools(self):
        self.git('init', '-q')
        not_inputs = ('tools/check_host.py', 'tools/build/check_patch_series.py',
                      'tools/package/85-piano-boot-space.install', 'tools/package/boot.py',
                      'tools/package/camera.py', 'tools/build_release_rootfs.py',
                      'tools/build_nyarch_rootfs.py', 'tools/build_steamos_rootfs.py')
        inputs = ('tools/build_product.sh', 'tools/build_android_product_module.py',
                  'tools/prepare_nv_runtime_guard.py')
        for name in (*not_inputs, *inputs):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('original\n')
        self.git('add', '.')
        with patch.object(shared, 'environment_identity', return_value={'container_image': 'sha256:a'}):
            _, original = shared.identity('firmware', self.root)
            for name in not_inputs:
                (self.root / name).write_text('edited\n')
                self.assertEqual(shared.identity('firmware', self.root)[1], original)
            for name in inputs:
                path = self.root / name
                path.write_text('edited\n')
                self.assertNotEqual(shared.identity('firmware', self.root)[1], original)
                path.write_text('original\n')

    def test_native_kernel_key_binds_boot_packaging_inputs(self):
        self.git('init', '-q')
        (self.root / 'config').mkdir()
        (self.root / 'tools').mkdir()
        (self.root / 'config/release.json').write_bytes((ROOT / 'config/release.json').read_bytes())
        helpers = ('tools/compose_piano_dtb.py', 'tools/prepare_boot_files.py',
                   'tools/package/85-piano-boot-space.install', 'tools/package/boot.py')
        not_inputs = ('tools/check_host.py', 'tools/prepare_nv_runtime_guard.py')
        for name in (*helpers, *not_inputs):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('original\n')
        self.git('add', '.')
        with patch.object(shared, 'environment_identity', return_value={'container_image': 'sha256:a'}):
            _, original = shared.identity('kernel', self.root)
            for name in not_inputs:
                (self.root / name).write_text('edited\n')
                self.assertEqual(shared.identity('kernel', self.root)[1], original)
            for name in helpers:
                path = self.root / name
                path.write_text('edited\n')
                self.assertNotEqual(shared.identity('kernel', self.root)[1], original)
                path.write_text('original\n')

    def test_kernel_key_binds_versions_but_not_the_project_head(self):
        self.git('init','-q')
        self.git('config','user.name','Fixture')
        self.git('config','user.email','fixture@example.invalid')
        directory=self.root/'config';directory.mkdir()
        path=directory/'release.json'
        release=json.loads((ROOT/'config/release.json').read_text())
        path.write_text(json.dumps(release,indent=2)+'\n')
        self.git('add','.')
        self.git('commit','-qm','kernel policy',epoch=1794983710)
        with patch.object(shared,'KERNEL_INPUTS',('config/release.json',)), \
                patch.object(shared,'environment_identity',return_value={'container_image':'sha256:builder'}):
            record,key=shared.identity('kernel',self.root)
            release['login']['autologin']=not release['login']['autologin']
            release['mesa']={'version':'27.0.0'}
            path.write_text(json.dumps(release,sort_keys=True))
            (self.root/'firmware.c').write_text('/* Firmware-only change. */\n')
            self.git('add','.')
            self.git('commit','-qm','new firmware and product configuration',epoch=1794984380)
            self.assertEqual(shared.identity('kernel',self.root),(record,key))
            release['kernel']['package_epoch']+=1
            path.write_text(json.dumps(release))
            self.assertNotEqual(shared.identity('kernel',self.root)[1],key)
            _, package_key=shared.identity('kernel',self.root)
            release['kernel']['abi_epoch']+=1
            path.write_text(json.dumps(release))
            self.assertNotEqual(shared.identity('kernel',self.root)[1],package_key)

    def test_kernel_restore_rejects_unknown_or_changed_policy_before_building(self):
        import build_piano_full_kernel as kernel
        (self.root/'config').mkdir()
        config=self.root/'config/release.json'
        release=json.loads((ROOT/'config/release.json').read_text())
        config.write_text(json.dumps(release))
        policy=kernel.kernel_policy_identity(self.root)
        artifact=self.root/'artifacts/kernels/release';artifact.mkdir(parents=True)
        source=self.root/'build/release';source.mkdir(parents=True)
        (source/'source-manifest.json').write_text(json.dumps({'actual_commit':'a'*40}))
        marker=artifact/'manifest.json'
        for metadata in ({},{'kernel_policy':policy['policy'],'kernel_policy_sha256':policy['sha256']}):
            marker.write_text(json.dumps({'source_commit':'a'*40,**metadata}))
            release['kernel']['patch_series']='patches/linux/next/series.json'
            config.write_text(json.dumps(release))
            before=marker.read_bytes()
            with patch.object(kernel,'toolchain') as toolchain,patch.object(shared.subprocess,'run') as command:
                with self.assertRaisesRegex(ValueError,'policy differs or lacks section provenance'):
                    shared.check_kernel_hot_inputs(self.root)
                toolchain.assert_not_called()
                command.assert_not_called()
            self.assertEqual(marker.read_bytes(),before)

    def test_failed_build_never_publishes_a_cache_receipt(self):
        with patch.object(shared, 'identity', return_value=({'kind': 'firmware'}, 'key')), \
                patch('prepare_sources.check'), \
                patch.object(shared.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'build')):
            with self.assertRaises(subprocess.CalledProcessError):
                shared.run('firmware', root=self.root)
        self.assertFalse((self.root / 'build/shared-products/firmware.json').exists())

    def test_matrix_builds_common_products_once_without_changing_root_targets(self):
        self.assertEqual(ci_build_targets.DISTROS,
                         ['debian-gnome', 'debian-kde', 'nyarch-gnome', 'steamos'])
        self.assertEqual(ci_build_targets.shared_kinds(ci_build_targets.DISTROS), ['firmware', 'kernel'])
        self.assertEqual(ci_build_targets.shared_kinds(['uefi']), ['firmware'])
        self.assertEqual(ci_build_targets.shared_kinds(['linux']), ['kernel'])
        self.assertEqual(ci_build_targets.shared_kinds([]), [])
        self.assertEqual(ci_build_targets.overlay_targets(ci_build_targets.DISTROS),
                         ['debian-gnome', 'nyarch-gnome', 'steamos'])
        self.assertEqual(ci_build_targets.overlay_targets(['debian-kde']), ['debian-gnome'])
        self.assertEqual(ci_build_targets.overlay_targets(['linux', 'uefi']), [])


if __name__ == '__main__':
    unittest.main()
