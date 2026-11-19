"""Check Debian source identity and package-cache ABI boundaries."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import debian_package


class DebianPackageTests(unittest.TestCase):
    def test_build_dependency_retry_requires_only_https_connection_resets(self):
        reset = ('E: Failed to fetch https://deb.debian.org/debian/pool/dh-python.deb  OpenSSL system '
                 'call error: error:80000068:system library::Connection reset by peer  - openssl '
                 '(104: Connection reset by peer) [IP: 192.0.2.1 443]')
        summary = '\nE: Some files failed to download\nE: Failed to process build dependencies\n'
        self.assertEqual(debian_package.dependency_download_resets(reset + summary),
                         {'https://deb.debian.org/debian/pool/dh-python.deb'})
        for error in (
            summary,
            reset.replace('https://', 'http://') + summary,
            reset.replace('(104: Connection reset by peer)', '(5: Input/output error)') + summary,
            reset + '\nE: Failed to fetch https://deb.debian.org/another.deb  Hash Sum mismatch' + summary,
            reset + "\nE: The repository 'https://example.invalid sid Release' is not signed." + summary,
            reset + "\nE: Can not find version '2' of package 'mutter'" + summary,
        ):
            with self.subTest(error=error):
                self.assertFalse(debian_package.dependency_download_resets(error))

    def test_mesa_local_revision_is_bound_to_config_and_built_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'config/mesa.json'
            producer = root / 'tools/build/mesa.py'
            output = root / 'build/mesa/SOURCE.json'
            for path in (config, producer, output):
                path.parent.mkdir(parents=True, exist_ok=True)
            policy = {'source': {'repository': 'https://gitlab.freedesktop.org/mesa/mesa.git',
                                 'commit': 'a' * 40, 'version': '27.0.0-devel', 'sha256': 'b' * 64},
                      'debian': {'recipe_source_version': '26.2.6-1',
                                 'package_version': '27.0.0~devel+git123-1'}}
            config.write_text(json.dumps(policy))
            producer.write_text('Mesa package builder')
            record = {'official_source': policy['source'], 'source': {'version': '26.2.6-1'},
                      'patches': [], 'config_sha256': debian_package.sha(config),
                      'producer_sha256': debian_package.sha(producer)}
            output.write_text(json.dumps(record))
            official = {'Package': 'libgbm1', 'Source': 'mesa', 'Version': '26.2.6-1'}
            installed = {'version': policy['debian']['package_version'], 'source': 'mesa',
                         'source_version': policy['debian']['package_version'], 'architecture': 'arm64'}
            with patch.object(debian_package, 'ROOT', root):
                self.assertEqual(debian_package.local_package_spec('libgbm1', installed, official),
                                 (root / 'build/mesa', []))
                with self.assertRaisesRegex(ValueError, 'declared local revision'):
                    debian_package.local_package_spec('libgbm1', {**installed, 'version': '99+piano1'}, official)
                record['official_source'] = {'version': 'different'}
                output.write_text(json.dumps(record))
                with self.assertRaisesRegex(ValueError, 'provenance'):
                    debian_package.local_package_spec('libgbm1', installed, official)

    def test_local_revision_needs_declared_package_source_and_exact_version(self):
        official = {'Package': 'gdm3', 'Source': 'gdm3 (51.1-1)', 'Version': '51.1-1+b1'}
        installed = {'version': '51.1-1+sunuefi2', 'source': 'gdm3',
                     'source_version': '51.1-1+sunuefi2', 'architecture': 'arm64'}
        folder, patches = debian_package.local_package_spec('gdm3', installed, official)
        self.assertEqual(folder, ROOT / 'build/gdm')
        self.assertEqual(len(patches), 2)
        for field, value in [('version', '51.1-1+sunuefi999'), ('source', 'unrelated'),
                             ('source_version', '51.0-1+sunuefi1'), ('architecture', 'amd64')]:
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'declared local revision'):
                debian_package.local_package_spec('gdm3', {**installed, field: value}, official)
        with self.assertRaisesRegex(ValueError, 'No declared local ABI revision'):
            debian_package.local_package_spec('gnome-shell', installed,
                                              {'Package': 'gnome-shell', 'Version': '51.0-3'})

    def test_local_pipewire_abi_must_use_the_declared_native_library(self):
        from desktop import build_pipewire_packages

        name = debian_package.PIPEWIRE_ABI_PACKAGE
        official = {'Package': name, 'Source': 'pipewire', 'Version': '1.6.9-2'}
        version = '1.6.9-2+sunuefi' + str(build_pipewire_packages.DEBIAN_REVISION)
        installed = {'version': version, 'source': 'pipewire',
                     'source_version': version, 'architecture': 'arm64'}
        output = ROOT / 'build/packages/pipewire'
        folder, patches = debian_package.local_package_spec(name, installed, official,
                                                            package_output=output)
        self.assertEqual(folder, output)
        self.assertIn('patches/pipewire/1.6.9/0009-client-node-empty-params.patch',
                      {row['path'] for row in patches})
        with self.assertRaisesRegex(ValueError, 'declared local revision'):
            debian_package.local_package_spec(name, {**installed, 'version': '1.6.9-2+unknown'}, official)

    def test_gbm_cache_comparison_preserves_actual_local_identity(self):
        baseline = {'package': 'libgbm1', 'version': '26.2.6-1', 'source': 'mesa',
                    'source_version': '26.2.6-1', 'sha256': 'signed-package-sha'}
        stock = {'suite': 'sid', 'architecture': 'arm64', 'abi': {'libgbm1': baseline}}
        actual = {**baseline, 'version': '27.0.0-devel-1', 'source_version': '27.0.0-devel-1',
                  'package_sha256': 'actual-local-sha', 'signed_baseline': baseline,
                  'verified_baseline_sha256': baseline['sha256']}
        local = {**stock, 'abi': {'libgbm1': actual},
                 'installed_local': {'libgbm1': {'package_sha256': 'actual-local-sha',
                                                'installed_elf_sha256': {'libgbm.so': 'elf-sha'}}}}
        before = copy.deepcopy(local)
        self.assertTrue(debian_package.same_target(stock, local))
        self.assertEqual(local, before)
        invalid = copy.deepcopy(local)
        invalid['abi']['libgbm1']['verified_baseline_sha256'] = 'different-baseline'
        self.assertFalse(debian_package.same_target(stock, invalid))
        invalid = copy.deepcopy(local)
        invalid.pop('installed_local')
        self.assertFalse(debian_package.same_target(stock, invalid))
        changed = copy.deepcopy(local)
        changed['installed_local']['libgbm1']['package_sha256'] = 'different-local-package'
        self.assertFalse(debian_package.same_target(local, changed))
        changed = copy.deepcopy(local)
        changed['abi']['libc6'] = {'version': 'different'}
        self.assertFalse(debian_package.same_target(stock, changed))

    def test_current_target_and_original_build_target_are_both_returned(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / 'runtime').mkdir()
            binary = output / 'runtime/gdm3.deb'
            binary.write_bytes(b'cached package')
            build_target = {'suite': 'sid', 'architecture': 'arm64', 'abi': {},
                            'binary': {'source': 'gdm3', 'source_version': '51.1-1'},
                            'source_checksums': {'gdm3.dsc': {'sha256': 'source', 'size': 1}}}
            target = {**build_target, 'installed_local': {'gdm3': {'version': '51.1-1+sunuefi1'}}}
            record = {'schema_version': 2, 'target': build_target, 'packages_built': True,
                      'package_version': '51.1-1+sunuefi1', 'patches': [], 'inputs': {},
                      'recipe': debian_package.BUILD_RECIPE, 'build_command': debian_package.BUILD_COMMAND,
                      'build_profiles': debian_package.BUILD_PROFILES,
                      'source': {'package': 'gdm3', 'version': '51.1-1', 'suite': 'sid', 'archives': [],
                                 'dsc': {'file': 'gdm3.dsc', 'sha256': 'source', 'size': 1}},
                      'packages': {'gdm3': {'file': binary.name, 'version': '51.1-1+sunuefi1',
                                           'architecture': 'arm64', 'sha256': debian_package.sha(binary)}}}
            metadata = output / 'SOURCE.json'
            original = json.dumps(record)
            metadata.write_text(original)
            result = debian_package.reusable(output, target, [], {'gdm3': 'arm64'}, {}, 1)
            self.assertEqual(result['target'], target)
            self.assertEqual(result['build_target'], build_target)
            self.assertEqual(metadata.read_text(), original)

    def test_binnmu_selects_original_source_version(self):
        self.assertEqual(debian_package.source_identity(
            {'Package': 'gdm3', 'Source': 'gdm3 (51.1-1)', 'Version': '51.1-1+b1'}),
            ('gdm3', '51.1-1'))
        self.assertEqual(debian_package.source_identity(
            {'Package': 'gnome-settings-daemon', 'Version': '51.0-2'}),
            ('gnome-settings-daemon', '51.0-2'))

    def test_failed_package_validation_keeps_the_build_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            marker = output / '.incomplete'
            marker.touch()
            manifest = output / 'SOURCE.json'
            manifest.write_text('{"package_version": "1"}\n')
            checksums = output / 'SHA256SUMS'
            checksums.write_text('previous  runtime/camera_1_arm64.deb\n')
            record = {'package_version': '2', 'packages_built': True,
                      'packages': {'camera': {'file': 'camera_2_arm64.deb', 'sha256': 'new'}}}

            def reject(folder, packages):
                self.assertEqual(folder, output)
                self.assertEqual(packages, record)
                self.assertTrue(marker.is_file())
                raise ValueError('The camera ABI does not match')

            with self.assertRaisesRegex(ValueError, 'camera ABI does not match'):
                debian_package.complete_build(output, record, reject)
            self.assertTrue(marker.is_file())
            self.assertEqual(json.loads(manifest.read_text()), {'package_version': '1'})
            self.assertEqual(checksums.read_text(), 'previous  runtime/camera_1_arm64.deb\n')

            debian_package.complete_build(output, record)
            self.assertFalse(marker.exists())
            self.assertEqual(json.loads(manifest.read_text()), record)
            self.assertEqual(checksums.read_text(), 'new  runtime/camera_2_arm64.deb\n')

    def test_cache_requires_exact_source_abi_and_complete_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            runtime = output / 'runtime'
            runtime.mkdir()
            binary = runtime / 'gdm3.deb'
            binary.write_bytes(b'package-cache-check')
            target = {'suite': 'sid', 'architecture': 'arm64',
                      'binary': {'source': 'gdm3', 'source_version': '51.1-1'},
                      'source_checksums': {'gdm3.dsc': {'sha256': 'source', 'size': 1}},
                      'abi': {'libc6': {'version': '2.42-1'}}}
            inputs = {'recipe': 'recipe-sha256'}
            record = {'schema_version': 2, 'packages_built': True, 'target': target, 'patches': [],
                      'source': {'package': 'gdm3', 'version': '51.1-1', 'suite': 'sid',
                                 'dsc': {'file': 'gdm3.dsc', 'sha256': 'source', 'size': 1}, 'archives': []},
                      'inputs': inputs,
                      'recipe': debian_package.BUILD_RECIPE,
                      'build_command': debian_package.BUILD_COMMAND,
                      'build_profiles': debian_package.BUILD_PROFILES,
                      'package_version': '51.1-1+sunuefi1',
                      'packages': {'gdm3': {'file': binary.name, 'version': '51.1-1+sunuefi1',
                                           'architecture': 'arm64', 'sha256': debian_package.sha(binary)}}}
            (output / 'SOURCE.json').write_text(json.dumps(record))

            def reusable(selected=target, selected_inputs=inputs, *, validate_packages=None):
                return debian_package.reusable(output, selected, [], {'gdm3': 'arm64'}, selected_inputs, 1,
                                               validate_packages=validate_packages)

            def require_payload(folder, cached):
                archive = folder / 'runtime' / cached['packages']['gdm3']['file']
                if archive.read_bytes() != b'package-cache-check':
                    raise ValueError('The package payload does not match')

            self.assertIsNotNone(reusable(validate_packages=require_payload))
            binary.write_bytes(b'package-with-wrong-payload')
            record['packages']['gdm3']['sha256'] = debian_package.sha(binary)
            (output / 'SOURCE.json').write_text(json.dumps(record))
            self.assertIsNotNone(reusable())
            self.assertIsNone(reusable(validate_packages=require_payload))
            self.assertFalse((output / '.incomplete').exists())
            self.assertEqual(json.loads((output / 'SOURCE.json').read_text()), record)

            for field, value in [('suite', 'trixie'), ('architecture', 'amd64'),
                                 ('source_checksums', {'gdm3.dsc': {'sha256': 'changed', 'size': 1}}),
                                 ('abi', {'libc6': {'version': '2.43-1'}})]:
                selected = copy.deepcopy(target)
                selected[field] = value
                with self.subTest(field=field):
                    self.assertIsNone(reusable(selected))
            self.assertIsNone(reusable(selected_inputs={'recipe': 'changed'}))
            verifier = Path(debian_package.__file__).relative_to(ROOT).as_posix()
            self.assertIsNotNone(reusable(selected_inputs={**inputs, verifier: 'verification-only-change'}))
            dependency = {'libcamera-dev': {'version': '0.8.0-1', 'architecture': 'arm64', 'sha256': 'native'}}
            self.assertIsNone(debian_package.reusable(
                output, target, [], {'gdm3': 'arm64'}, inputs, 1, build_dependencies=dependency))
            record['build_dependencies'] = dependency
            (output / 'SOURCE.json').write_text(json.dumps(record))
            self.assertIsNone(reusable())
            self.assertIsNotNone(debian_package.reusable(
                output, target, [], {'gdm3': 'arm64'}, inputs, 1, build_dependencies=dependency))
            changed = {'libcamera-dev': {**dependency['libcamera-dev'], 'sha256': 'changed'}}
            self.assertIsNone(debian_package.reusable(
                output, target, [], {'gdm3': 'arm64'}, inputs, 1, build_dependencies=changed))
            record.pop('build_dependencies')
            record['build_recipe_version'] = debian_package.BUILD_RECIPE_VERSION + 1
            (output / 'SOURCE.json').write_text(json.dumps(record))
            self.assertIsNone(reusable())
            record.pop('build_recipe_version')
            (output / 'SOURCE.json').write_text(json.dumps(record))
            record['build_command'] = ['dpkg-buildpackage', '-b', '-us', '-uc', '-d']
            (output / 'SOURCE.json').write_text(json.dumps(record))
            self.assertIsNone(reusable())
            record['build_command'] = debian_package.BUILD_COMMAND
            (output / 'SOURCE.json').write_text(json.dumps(record))
            (output / '.incomplete').touch()
            self.assertIsNone(reusable())
            (output / '.incomplete').unlink()
            binary.write_bytes(b'changed')
            self.assertIsNone(reusable())
