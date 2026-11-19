"""Build tiny signed Denial releases with real Debian and GnuPG tools."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import denial_overlay
from desktop import denial_packages

PREFIX = '' if os.uname().machine == 'aarch64' else 'aarch64-linux-gnu-'
TOOLS = ('dpkg', 'dpkg-deb', 'gpg', 'gpgv', 'gpgconf', 'readelf', PREFIX + 'as', PREFIX + 'ld')
MAIN = 'denial'
PLUGIN = 'denial-plugin-manager'
ENGINE = 'denial-flutter-engine'
PROGRAM = 'usr/bin/deniald'
CONFFILE = 'etc/denial/session.conf'
CONFIGURATION = 'DENIAL_SHELL_PROFILE=mobile\n'
RELEASE_EPOCH = 1_700_000_000


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(all(map(shutil.which, TOOLS)), 'Debian, GnuPG and ARM64 binutils tools required')
class DenialPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-denial-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        environment = patch.dict(os.environ, LC_ALL='C')
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('SUNUEFI_OVERLAY_RELEASE', None)
        os.environ.pop('SOURCE_DATE_EPOCH', None)

        self.source = json.loads((ROOT / 'config/denial.json').read_text())['source']
        version = self.source['tag'].removeprefix('v')
        self.upstream_version = version + '-1'
        series = ROOT / 'patches/denial' / version / 'series.json'
        self.patch_hashes = {item['file']: sha(series.parent / item['file'])
                             for item in json.loads(series.read_text())['patches']}
        self.binary = self.build_binary()
        self.receipt = self.root / 'SOURCE.json'
        self.receipt.write_text(json.dumps({'source': self.source, 'patches': self.patch_hashes,
                                            'binary_sha256': sha(self.binary)}))

        gnupg = self.root / 'gnupg'
        gnupg.mkdir(mode=0o700)
        self.gpg = ['gpg', '--no-options', '--homedir', gnupg, '--batch', '--yes']
        self.addCleanup(subprocess.run, ['gpgconf', '--homedir', gnupg, '--kill', 'gpg-agent'],
                        capture_output=True)
        subprocess.run([*self.gpg, '--faked-system-time', f'{RELEASE_EPOCH - 2}!',
                        '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Denial Release Test <test@example.invalid>',
                        'ed25519', 'sign', '0'], check=True, capture_output=True)
        records = subprocess.check_output([*self.gpg, '--with-colons', '--list-keys'], text=True)
        self.fingerprint = next(line.split(':')[9] for line in records.splitlines()
                                if line.startswith('fpr:'))
        public_key = self.root / 'denial-test.asc'
        public_key.write_bytes(subprocess.check_output(
            [*self.gpg, '--armor', '--export', self.fingerprint]))
        keys = patch.multiple(denial_packages, KEY=public_key, FINGERPRINT=self.fingerprint)
        keys.start()
        self.addCleanup(keys.stop)

        self.upstream = self.root / 'upstream'
        self.upstream.mkdir()
        self.archives = {name: self.deb(name) for name in (MAIN, PLUGIN, ENGINE)}
        self.checksums = self.upstream / 'SHA256SUMS'
        self.sign_checksums()

    def build_binary(self):
        obj = self.root / 'deniald.o'
        subprocess.run([PREFIX + 'as', '-o', obj, '-'],
                       input=b'.text\n.global _start\n_start:\n  mov x8, #93\n  svc #0\n',
                       check=True, capture_output=True)
        lib = self.root / 'libseat.so.1'
        subprocess.run([PREFIX + 'ld', '-shared', '-soname', lib.name, '-o', lib, obj],
                       check=True, capture_output=True)
        binary = self.root / 'deniald'
        subprocess.run([PREFIX + 'ld', '-pie', '--no-as-needed', '-o', binary, obj, lib],
                       check=True, capture_output=True)
        return binary

    def sign(self, path, epoch):
        subprocess.run([*self.gpg, '--faked-system-time', f'{epoch}!',
                        '--local-user', self.fingerprint,
                        '--detach-sign', path], check=True, capture_output=True)

    def deb(self, name):
        tree = self.root / name
        control = tree / 'DEBIAN/control'
        control.parent.mkdir(parents=True)
        fields = (f'Package: {name}\nVersion: {self.upstream_version}\nArchitecture: arm64\n'
                  'Maintainer: Denial Release Test <test@example.invalid>\n'
                  'Installed-Size: 1\nDescription: Denial package fixture\n')
        if name == MAIN:
            fields += (f'Depends: bash, {ENGINE} (= {self.upstream_version})\n'
                       f'Suggests: {PLUGIN} (= {self.upstream_version}), ddcutil\n')
            program = tree / PROGRAM
            program.parent.mkdir(parents=True)
            program.write_bytes(b'#!/bin/sh\nexit 0\n')
            program.chmod(0o755)
            digest = hashlib.md5(program.read_bytes(), usedforsecurity=False).hexdigest()
            (control.parent / 'md5sums').write_text(f'{digest}  {PROGRAM}\n')
            config = tree / CONFFILE
            config.parent.mkdir(parents=True)
            config.write_text(CONFIGURATION)
            (control.parent / 'conffiles').write_text('/' + CONFFILE + '\n')
        elif name == PLUGIN:
            fields += f'Depends: {MAIN} (= {self.upstream_version}), git\n'
        control.write_text(fields)
        archive = self.upstream / f'{name}_{self.upstream_version}_arm64.deb'
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', tree, archive],
                       check=True, capture_output=True)
        self.sign(archive, RELEASE_EPOCH - 1)
        return archive

    def sign_checksums(self):
        files = [file for archive in self.archives.values()
                 for file in (archive, archive.with_name(archive.name + '.sig'))]
        self.checksums.write_text(''.join(f'{sha(file)}  {file.name}\n' for file in files))
        self.sign(self.checksums, RELEASE_EPOCH)

    def build(self, output, **kwargs):
        return denial_packages.build(self.binary, self.receipt, self.upstream, output, 'debian', **kwargs)

    def field(self, archive, field):
        return subprocess.check_output(['dpkg-deb', '-f', archive, field], text=True).strip()

    def test_builds_one_native_upgrade_without_replacing_the_engine(self):
        engine = self.archives[ENGINE]
        engine_digest = sha(engine)
        output = self.root / 'packages'
        with patch.dict(os.environ, SUNUEFI_OVERLAY_RELEASE='123.4'):
            record = self.build(output, target='debian-denial')

        version = self.upstream_version + '+sunuefi.123.4'
        archives = {name: output / f'{name}_{version}_arm64.deb' for name in (MAIN, PLUGIN)}
        manifest = output / 'manifest.json'
        self.assertEqual(set(output.iterdir()), {manifest, *archives.values()})
        self.assertEqual(json.loads(manifest.read_text()), record)
        self.assertEqual(record['overlay_release'], '123.4')
        self.assertEqual((record['target'], record['repo']), ('debian-denial', 'apt'))
        self.assertEqual(record['upstream_release']['archives'][ENGINE]['sha256'], engine_digest)
        self.assertEqual(sha(engine), engine_digest)
        for name, archive in archives.items():
            self.assertEqual(self.field(archive, 'Package'), name)
            self.assertEqual(self.field(archive, 'Version'), version)
            self.assertEqual(self.field(archive, 'Architecture'), 'arm64')
            self.assertEqual(record['packages'][archive.name]['sha256'], sha(archive))
        subprocess.run(['dpkg', '--compare-versions', version, 'gt', self.upstream_version],
                       check=True)

        main, plugin = archives[MAIN], archives[PLUGIN]
        self.assertEqual(self.field(main, 'Depends'), f'bash, {ENGINE} (= {self.upstream_version})')
        self.assertEqual(self.field(main, 'Suggests'), f'{PLUGIN} (= {version}), ddcutil')
        self.assertEqual(self.field(plugin, 'Depends'), f'{MAIN} (= {version}), git')
        unpacked = self.root / 'unpacked'
        subprocess.run(['dpkg-deb', '--raw-extract', main, unpacked],
                       check=True, capture_output=True)
        program = unpacked / PROGRAM
        self.assertEqual(program.read_bytes(), self.binary.read_bytes())
        self.assertTrue(os.access(program, os.X_OK))
        self.assertEqual((unpacked / CONFFILE).read_text(), CONFIGURATION)
        self.assertEqual((unpacked / 'DEBIAN/conffiles').read_text(), '/' + CONFFILE + '\n')
        digest = hashlib.md5(self.binary.read_bytes(), usedforsecurity=False).hexdigest()
        self.assertEqual((unpacked / 'DEBIAN/md5sums').read_text(), f'{digest}  {PROGRAM}\n')

    def test_fetches_only_the_selected_assets_and_verifies_cached_inputs(self):
        location = self.upstream.as_uri() + '/'
        with (patch.object(denial_overlay, 'ROOT', self.root),
              patch.object(denial_overlay, 'release_url', return_value=location)):
            cached = denial_overlay.fetch_archives(self.source, 'debian')
            asset = self.archives[MAIN].name
            self.assertEqual(sha(cached / asset), sha(self.upstream / asset))
            self.assertEqual({path.name for path in cached.iterdir()}, {
                'download.lock', 'SHA256SUMS', 'SHA256SUMS.sig',
                *(path.name for path in self.archives.values()),
                *(path.name + '.sig' for path in self.archives.values()),
            })
            (cached / asset).write_bytes(b'modified download')
            with self.assertRaisesRegex(ValueError, 'Cached input checksum mismatch'):
                denial_overlay.fetch_archives(self.source, 'debian')

    def test_verify_build_rejects_a_changed_binary(self):
        proof, version = denial_packages.build_source(self.binary, self.receipt)
        self.assertEqual(proof['binary_sha256'], sha(self.binary))
        self.assertEqual(version, self.source['tag'].removeprefix('v'))
        self.binary.write_bytes(self.binary.read_bytes() + b'extra')
        with self.assertRaisesRegex(ValueError, 'does not match the build record'):
            denial_packages.build_source(self.binary, self.receipt)

    def test_rebuild_uses_the_signed_release_time_for_both_archives(self):
        builds = []
        for offset in (60, 120):
            output = self.root / f'packages-{offset}'
            with patch.dict(os.environ, SOURCE_DATE_EPOCH=str(RELEASE_EPOCH + offset)):
                record = self.build(output)
            self.assertEqual(record['upstream_release']['source_date_epoch'], RELEASE_EPOCH)
            self.assertEqual(len(record['packages']), 2)
            hashes = {}
            for name in record['packages']:
                archive = output / name
                with archive.open('rb') as stream:
                    self.assertEqual(stream.read(8), b'!<arch>\n')
                    header = stream.read(60)
                # The ar member timestamp follows its 16-byte file identifier.
                self.assertEqual(int(header[16:28]), RELEASE_EPOCH)
                hashes[name] = sha(archive)
            builds.append(hashes)
        self.assertEqual(builds[0], builds[1])

    def test_rejects_a_modified_archive_and_release_signature(self):
        output = self.root / 'packages'
        for signed, message in ((self.archives[MAIN], 'differs from signed SHA256SUMS'),
                                (self.upstream / 'SHA256SUMS.sig', 'Invalid Denial release signature')):
            with self.subTest(file=signed.name):
                original = signed.read_bytes()
                signed.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                try:
                    with self.assertRaisesRegex(ValueError, message):
                        self.build(output)
                    self.assertFalse(output.exists())
                finally:
                    signed.write_bytes(original)

    def test_requires_the_signature_of_each_package_even_with_signed_checksums(self):
        main_signature = self.archives[MAIN].with_suffix('.deb.sig')
        plugin_signature = self.archives[PLUGIN].with_suffix('.deb.sig')
        main_signature.write_bytes(plugin_signature.read_bytes())
        self.sign_checksums()

        output = self.root / 'packages'
        with self.assertRaisesRegex(ValueError, 'Invalid Denial release signature: denial_'):
            self.build(output)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
