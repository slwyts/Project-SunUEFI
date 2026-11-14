#!/usr/bin/env python3
"""Check signed-source metadata parsing without network or a device."""
from pathlib import Path
import sys
import json
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from build import debian


class DebianSourceTests(unittest.TestCase):
    def candidate_records(self):
        return [{'Package': 'libgbm1', 'Architecture': 'arm64', 'Source': 'mesa',
                 'Version': version, 'SHA256': version} for version in ('26.2.6-1', '26.1.6-1')]

    def signed_parser(self):
        return patch.dict(sys.modules, {'desktop.debian_package': SimpleNamespace(
            source_identity=lambda row: (row.get('Source', row['Package']), row['Version']))})

    def test_candidate_respects_pin_instead_of_highest_version(self):
        with self.signed_parser():
            row = debian.official_candidate('libgbm1', '26.1.6-1', self.candidate_records(), {})
        self.assertEqual(row['Version'], '26.1.6-1')

    def test_local_candidate_requires_known_revision_and_matching_archive_policy(self):
        libraries = {'libssc2': {'version': '0.4.4-2+piano1+sunuefi3', 'source': 'libssc'}}
        records = [{'Package': 'libssc2', 'Version': version, 'Source': 'libssc',
                    'Architecture': 'arm64', 'SHA256': version} for version in ('0.4.4-2', '0.4.4-1')]
        with self.signed_parser():
            row = debian.official_candidate('libssc2', libraries['libssc2']['version'], records,
                                            libraries, '0.4.4-2')
            self.assertEqual(row['Version'], '0.4.4-2')
            with self.assertRaisesRegex(ValueError, 'unambiguously'):
                debian.official_candidate('libssc2', libraries['libssc2']['version'], records,
                                            libraries, '0.4.4-1')
            libraries['libssc2']['version'] = '0.4.4-2+piano-untrusted'
            with self.assertRaisesRegex(ValueError, 'known project revision'):
                debian.official_candidate('libssc2', libraries['libssc2']['version'],
                                            records, libraries, '0.4.4-2')

    def test_unrelated_local_abi_is_not_exempted(self):
        libraries = {'libc6': {'version': '1+piano1', 'source': 'glibc'}}
        records = [{'Package': 'libc6', 'Architecture': 'arm64', 'Version': '2',
                    'Source': 'glibc', 'SHA256': 'signed'}]
        with self.signed_parser(), self.assertRaisesRegex(ValueError, 'Refresh the target sid ABI'):
            debian.official_candidate('libc6', '2', records, libraries)

    def test_mesa_development_version_has_no_version_only_signed_alias(self):
        policy = json.loads((debian.ROOT / 'config/mesa.json').read_text())
        version = policy['debian']['package_version']
        libraries = {'libgbm1': {'version': version, 'source': 'mesa'}}
        with self.signed_parser():
            self.assertIsNone(debian.project_source_version('mesa', version))
            with self.assertRaisesRegex(ValueError, 'known project revision'):
                debian.official_candidate('libgbm1', version, self.candidate_records(),
                                         libraries, policy['debian']['recipe_source_version'])

    def test_local_gbm_requires_unambiguous_signed_packaging_source(self):
        installed = ('version: development\nsource: mesa\nsource_version: development\n'
                     'architecture: arm64\n')
        official = self.candidate_records()[0]
        rows = [{'Package': 'mesa', 'Version': official['Version'],
                 'Checksums-Sha256': digest + ' 1 mesa.dsc'} for digest in ('first', 'different')]
        for selected in ([], rows):
            signed = SimpleNamespace(
                source_identity=lambda row: ('mesa', row['Version']),
                signed_records=lambda *args: (selected, []))
            with patch.dict(sys.modules, {'desktop.debian_package': signed}), \
                    patch.object(debian, 'capture', return_value=installed), \
                    self.assertRaisesRegex(ValueError, 'GBM packaging source'):
                debian.verify_local_gbm(Path('/target'), official)

    def test_batch_policy_preserves_epoch_and_archive_only_flag(self):
        text = 'ffmpeg:\n  Candidate: 7:9.0.2-6+b1\nlibc6:arm64:\n  Candidate: 2.43-7\n'
        with patch.object(debian, 'capture', return_value=text) as capture:
            self.assertEqual(debian.apt_candidates(Path('/builder'), ['libc6', 'ffmpeg'], True),
                             {'ffmpeg': '7:9.0.2-6+b1', 'libc6': '2.43-7'})
        self.assertIn('Dir::State::status=/dev/null', capture.call_args.args[0])

    def test_main_only_source_config_adds_contrib_sources_and_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'etc/apt').mkdir(parents=True)
            (root / 'etc/apt/sources.list').write_text(
                'deb https://deb.debian.org/debian sid main\n'
                'deb-src https://deb.debian.org/debian sid main\n')
            debian.build_sources(root)
            text = (root / 'etc/apt/sources.list.d/sunuefi-build.sources').read_text()
            rows = debian.controls(text)
            self.assertEqual({row['Types']: row['Components'] for row in rows},
                             {'deb': 'contrib', 'deb-src': 'contrib'})
            self.assertTrue(all(row['Signed-By'].endswith('debian-archive-keyring.gpg') for row in rows))

    def test_verifier_change_rechecks_signed_sources_without_rewriting_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / 'runtime').mkdir()
            (output / 'runtime/libgbm1.deb').write_bytes(b'cached package')
            source_checksums = {'mesa.dsc': {'sha256': 'original-source', 'size': 7}}
            target = debian.TargetIdentity({'suite': 'sid', 'architecture': 'arm64',
                'sources': {'mesa': '26.2.6-1'}, 'libraries': {}, 'binaries': {}}, Path('/target'))
            record = {'packages_built': True, 'target': dict(target), 'patches': [],
                'package_version': '26.2.6-1+piano1', 'recipe': 'original Debian debian/rules and control',
                'build_packages': {'libc6': '2.43-7'}, 'producer': 'tools/build/mesa.py',
                'producer_sha256': debian.package_build.sha(debian.ROOT / 'tools/build/mesa.py'),
                'builder_sha256': 'previous-builder-module', 'verifier_sha256': 'previous-verifier-module',
                'compiler_environment_sha256': debian.package_build.sha(debian.package_build.__file__),
                'source': {'package': 'mesa', 'version': '26.2.6-1', 'suite': 'sid',
                           'recipe': {'debian/rules': 'rules', 'debian/control': 'control'},
                           'apt_indexes': ['authenticated'],
                           'archives': [{'file': 'mesa.dsc', **source_checksums['mesa.dsc']}]},
                'packages': {'libgbm1': {'file': 'libgbm1.deb', 'architecture': 'arm64',
                                       'version': '26.2.6-1+piano1',
                                       'sha256': debian.package_build.sha(output / 'runtime/libgbm1.deb')}}}
            manifest = output / 'SOURCE.json'
            manifest.write_text(json.dumps(record))
            original = manifest.read_bytes()
            with patch.object(debian, 'source_checksums', return_value={'mesa': source_checksums}) as verify:
                self.assertIsNotNone(debian.reusable(output, record['package_version'], [], {'libgbm1'}, target))
                verify.assert_called_once_with(Path('/target'), target['sources'])
            self.assertEqual(manifest.read_bytes(), original)
            with patch.object(debian, 'source_checksums', return_value={'mesa': {}}):
                self.assertIsNone(debian.reusable(output, record['package_version'], [], {'libgbm1'}, target))
            record['build_recipe_version'] = debian.BUILD_RECIPE_VERSION + 1
            manifest.write_text(json.dumps(record))
            self.assertIsNone(debian.reusable(output, record['package_version'], [], {'libgbm1'}, target))

    def test_clearsigned_dsc_keeps_only_source_body(self):
        text = '''-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: ffmpeg
Version: 7:9.0.2-6
Checksums-Sha256:
 abcdef 42 ffmpeg_9.0.2.orig.tar.xz
 012345 24 ffmpeg_9.0.2-6.debian.tar.xz

-----BEGIN PGP SIGNATURE-----
Version: Not a Debian source field

signature
-----END PGP SIGNATURE-----
'''
        rows = [row for row in debian.controls(text) if 'Source' in row]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['Version'], '7:9.0.2-6')
        self.assertEqual(debian.checksum_rows(rows[0]), {
            'ffmpeg_9.0.2.orig.tar.xz': {'sha256': 'abcdef', 'size': 42},
            'ffmpeg_9.0.2-6.debian.tar.xz': {'sha256': '012345', 'size': 24}})

    def test_multiple_source_versions_remain_distinct(self):
        rows = debian.controls('Package: mesa\nVersion: 26.1.6-1\n\n'
                               'Package: mesa\nVersion: 26.2.6-1\n')
        self.assertEqual([row['Version'] for row in rows], ['26.1.6-1', '26.2.6-1'])

    def test_resume_rejects_changed_target_or_patch(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            target = {'suite': 'sid', 'architecture': 'arm64', 'sources': {'ffmpeg': '7:9.0.2-6'},
                      'binaries': {'libc6': {'version': '2.43-7'}}}
            patches = [{'path': 'fix.patch', 'sha256': 'abc'}]
            record = {'target': target, 'patches': patches, 'package_version': '7:9.0.2-6+sunuefi4'}
            (output / 'SOURCE.json').write_text(json.dumps(record))
            debian.validate_resume(output, target, patches, record['package_version'])
            changed = {**target, 'architecture': 'amd64'}
            with self.assertRaisesRegex(ValueError, 'target ABI'):
                debian.validate_resume(output, changed, patches, record['package_version'])
            with self.assertRaisesRegex(ValueError, 'package version and patches'):
                debian.validate_resume(output, target, [], record['package_version'])

    def test_unchanged_quilt_series_keeps_its_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / 'debian/patches').mkdir(parents=True)
            series = source / 'debian/patches/series'
            series.write_text('upstream.patch\n')
            (source / 'debian/changelog').write_text('mesa (26.2.6-1+piano1) sid; urgency=medium\n')
            local = source / 'fix.patch'
            local.write_text('patch content\n')
            debian.add_patches(source, [local], 'mesa', '26.2.6-1+piano1', 'Recognise the GPU.')
            timestamp = series.stat().st_mtime_ns
            debian.add_patches(source, [local], 'mesa', '26.2.6-1+piano1', 'Recognise the GPU.')
            self.assertEqual(series.stat().st_mtime_ns, timestamp)


if __name__ == '__main__':
    unittest.main()
