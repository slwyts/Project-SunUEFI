"""Verify publication inputs using native package metadata and detached signatures."""
from copy import deepcopy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/repo'))
from products import overlay_matrix, overlay_set, publication_batches, sha
from build import denial, denial_manifest
from desktop import denial_packages

DEBIAN = 'debian-denial'
ARCH = 'arch-denial'
MAIN, PLUGIN, ENGINE = 'denial', 'denial-plugin-manager', 'denial-flutter-engine'
ABI = 'denial-flutter-engine-abi=3.47.5.denial1'
TOOLS = ('dpkg-deb', 'bsdtar', 'gpg', 'gpgv', 'gpgconf')


class ManifestFixtures(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-denial-manifest-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = json.loads((ROOT / 'config/denial.json').read_text())['source']
        release = self.source['tag'].removeprefix('v')
        series = ROOT / 'patches/denial' / release / 'series.json'
        self.patches = {item['file']: sha(series.parent / item['file'])
                        for item in json.loads(series.read_text())['patches']}
        self.previous = release + '-1'

    def manifest(self, target):
        debian = target == DEBIAN
        architecture = 'arm64' if debian else 'aarch64'

        def filename(name, version):
            if debian:
                return f'{name}_{version}_{architecture}.deb'
            return f'{name}-{version.replace(":", ".")}-{architecture}.pkg.tar.zst'

        rebuilt = self.previous + ('+sunuefi.123.4' if debian else '.1000000000000000001230000000004')
        engine = self.previous if debian else '1:' + self.previous
        archives = {}
        packages = {}
        for name in (MAIN, PLUGIN, ENGINE):
            upstream = engine if name == ENGINE else self.previous
            archives[name] = {'file': filename(name, upstream), 'version': upstream,
                              'sha256': 'a' * 64, 'signature_sha256': 'b' * 64}
            if name != ENGINE or not debian:
                version = engine if name == ENGINE else rebuilt
                packages[filename(name, version)] = {
                    'name': name, 'version': version, 'architecture': architecture, 'sha256': 'a' * 64,
                }
        return {'schema_version': 1, 'target': target, 'repo': 'apt' if debian else 'sunuefi',
                'architecture': architecture, 'package_format': 'debian' if debian else 'pacman',
                'overlay_release': '123.4', 'source': self.source, 'patches': self.patches,
                'binary_sha256': 'c' * 64, 'packages': packages, 'upstream_release': {
                    'signing_key': denial.FINGERPRINT, 'sha256': 'd' * 64,
                    'signature_sha256': 'e' * 64, 'source_date_epoch': 1700000000, 'archives': archives}}

    def write(self, directory, manifest):
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / 'manifest.json'
        path.write_text(json.dumps(manifest))
        return path


class DenialManifestTests(ManifestFixtures):
    def test_reads_images_manifest_without_any_archives(self):
        for target in (DEBIAN, ARCH):
            with self.subTest(target=target):
                record = self.manifest(target)
                path = self.write(self.root / target, record)
                self.assertEqual(denial_manifest.read_manifest(path, target), record)

    def test_rejects_wrong_source_revision_or_engine(self):
        record = self.manifest(ARCH)
        path = self.write(self.root / ARCH, record)
        for key, value, error in (
                ('repo', 'sunuefi-steamos', 'does not match arch-denial'),
                ('source', {**self.source, 'commit': '0' * 40}, 'pinned source and patches'),
                ('patches', {}, 'pinned source and patches'),
                ('overlay_release', '123.5', 'complete native package set')):
            with self.subTest(key=key):
                changed = {**record, key: value}
                self.write(path.parent, changed)
                with self.assertRaisesRegex(ValueError, error):
                    denial_manifest.read_manifest(path, ARCH)
        changed = deepcopy(record)
        engine = changed['upstream_release']['archives'][ENGINE]
        changed['packages'][engine['file']]['sha256'] = '0' * 64
        self.write(path.parent, changed)
        with self.assertRaisesRegex(ValueError, 'unmodified upstream archive'):
            denial_manifest.read_manifest(path, ARCH)


@unittest.skipUnless(all(map(shutil.which, TOOLS)), 'Debian, bsdtar and GnuPG tools required')
class DenialPublicationTests(ManifestFixtures):
    def setUp(self):
        super().setUp()
        key_home = self.root / 'gnupg'
        key_home.mkdir(mode=0o700)
        self.addCleanup(subprocess.run, ['gpgconf', '--homedir', key_home, '--kill', 'gpg-agent'],
                        capture_output=True)
        self.gpg = ['gpg', '--no-options', '--homedir', key_home, '--batch', '--yes']
        subprocess.run([*self.gpg, '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Denial Archive Test <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        keys = subprocess.check_output([*self.gpg, '--with-colons', '--list-keys'], text=True)
        fingerprint = next(row.split(':')[9] for row in keys.splitlines() if row.startswith('fpr:'))
        public = self.root / 'upstream.asc'
        public.write_bytes(subprocess.check_output([*self.gpg, '--armor', '--export', fingerprint]))
        for module, fields in (
                (denial, {'FINGERPRINT': fingerprint}),
                (denial_packages, {'KEY': public, 'FINGERPRINT': fingerprint})):
            replacement = patch.multiple(module, **fields)
            replacement.start()
            self.addCleanup(replacement.stop)

    def sign(self, archive, output):
        subprocess.run([*self.gpg, '--output', output, '--detach-sign', archive],
                       check=True, capture_output=True)

    def archives(self, target, changes=None):
        directory = self.root / 'overlays' / target
        manifest = self.manifest(target)
        self.write(directory, manifest)
        engine = manifest['upstream_release']['archives'][ENGINE]
        version = next(row['version'] for row in manifest['packages'].values() if row['name'] == MAIN)
        for filename, row in manifest['packages'].items():
            archive = directory / filename
            name = row['name']
            if target == DEBIAN:
                deps = {MAIN: f'Depends: {ENGINE} (= {engine["version"]})\nSuggests: {PLUGIN} (= {version})\n',
                        PLUGIN: f'Depends: {MAIN} (= {version})\n'}
            else:
                deps = {MAIN: f'depend = {ENGINE}\ndepend = {ABI}\n',
                        PLUGIN: f'depend = {MAIN}={version}\n', ENGINE: f'provides = {ABI}\n'}
            dependencies = (changes or {}).get(name, deps[name])
            with tempfile.TemporaryDirectory(dir=self.root) as work:
                tree = Path(work)
                payload = []
                if name == MAIN:
                    program = tree / denial.COMPOSITOR.lstrip('/')
                    program.parent.mkdir(parents=True)
                    program.write_bytes(b'Package mirror fixture')
                    manifest['binary_sha256'] = sha(program)
                    payload.append(program.relative_to(tree))
                if target == DEBIAN:
                    control = tree / 'DEBIAN/control'
                    control.parent.mkdir()
                    control.write_text(
                        f'Package: {name}\nVersion: {row["version"]}\nArchitecture: {row["architecture"]}\n'
                        f'Maintainer: Test <test@example.invalid>\nDescription: Test\n{dependencies}')
                    command = ['dpkg-deb', '--root-owner-group', '--build', tree, archive]
                else:
                    (tree / '.PKGINFO').write_text(
                        f'pkgname = {name}\npkgver = {row["version"]}\narch = {row["architecture"]}\n'
                        f'{dependencies}')
                    command = ['bsdtar', '-caf', archive, '-C', tree, '.PKGINFO', *payload]
                subprocess.run(command, check=True, capture_output=True)
            row['sha256'] = sha(archive)
            if name == ENGINE:
                signature = archive.with_name(filename + '.sig')
                self.sign(archive, signature)
                engine.update(sha256=row['sha256'], signature_sha256=sha(signature))
        self.write(directory, manifest)
        return directory, manifest

    def test_batches_denial_without_other_overlay_requirements(self):
        allowed = {}
        for target in (DEBIAN, ARCH):
            directory, record = self.archives(target)
            allowed[record['repo']] = {directory / filename: info['sha256']
                                       for filename, info in record['packages'].items()}
        batch = publication_batches(self.root, kernel=False, overlays=overlay_matrix(
            '{"target":["debian-denial","arch-denial"]}'))
        self.assertEqual(batch, allowed)

    def test_rejects_debian_deps_even_when_archive_digests_match(self):
        for name, dependency, error in (
                (MAIN, f'Depends: {ENGINE} (= {self.previous}) | other-engine\n', 'must declare Depends'),
                (PLUGIN, f'Depends: {MAIN} (= {self.previous})\n', 'must declare Depends')):
            with self.subTest(name=name):
                directory, _ = self.archives(DEBIAN, {name: dependency})
                with self.assertRaisesRegex(ValueError, error):
                    overlay_set(directory, DEBIAN)

    def test_rejects_different_arch_abi_or_old_plugin(self):
        for name, dependency, error in (
                (MAIN, f'depend = {ENGINE}\ndepend = denial-flutter-engine-abi=other\n', 'same Flutter Engine ABI'),
                (PLUGIN, f'depend = {MAIN}={self.previous}\n', 'must depend on denial=')):
            with self.subTest(name=name):
                directory, _ = self.archives(ARCH, {name: dependency})
                with self.assertRaisesRegex(ValueError, error):
                    overlay_set(directory, ARCH)

    def test_rejects_archive_with_wrong_compositor(self):
        for target in (DEBIAN, ARCH):
            with self.subTest(target=target):
                directory, record = self.archives(target)
                record['binary_sha256'] = '0' * 64
                self.write(directory, record)
                with self.assertRaisesRegex(ValueError, 'compositor differs from the build record'):
                    overlay_set(directory, target)

    def test_checks_original_engine_signature_and_complete_directory(self):
        directory, record = self.archives(ARCH)
        engine = record['upstream_release']['archives'][ENGINE]
        signature = directory / (engine['file'] + '.sig')
        main = directory / next(filename for filename, row in record['packages'].items() if row['name'] == MAIN)
        self.sign(main, signature)
        engine['signature_sha256'] = sha(signature)
        self.write(directory, record)
        with self.assertRaisesRegex(ValueError, 'Invalid Denial release signature'):
            overlay_set(directory, ARCH)

        signature.unlink()
        with self.assertRaisesRegex(ValueError, 'exactly the recorded packages and signatures'):
            overlay_set(directory, ARCH)


if __name__ == '__main__':
    unittest.main()
