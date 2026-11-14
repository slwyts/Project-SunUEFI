"""Check Debian source identity and package-cache ABI boundaries."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import debian_package


class DebianPackageTests(unittest.TestCase):
    def test_binnmu_selects_original_source_version(self):
        self.assertEqual(debian_package.source_identity(
            {'Package': 'gdm3', 'Source': 'gdm3 (51.1-1)', 'Version': '51.1-1+b1'}),
            ('gdm3', '51.1-1'))
        self.assertEqual(debian_package.source_identity(
            {'Package': 'gnome-settings-daemon', 'Version': '51.0-2'}),
            ('gnome-settings-daemon', '51.0-2'))

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

            def reusable(selected=target, selected_inputs=inputs):
                return debian_package.reusable(output, selected, [], {'gdm3': 'arm64'}, selected_inputs, 1)

            self.assertIsNotNone(reusable())
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
