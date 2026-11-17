"""Sign real package indexes, merge releases and reject changed package bytes."""
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('repo_publish', ROOT / 'tools/repo/publish.py')
repo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repo)


class MemoryStore:
    def __init__(self):
        self.objects = {}
        self.writes = []

    def get(self, key, path):
        if key not in self.objects:
            return False
        path.write_bytes(self.objects[key])
        return True

    def put(self, key, path, *, immutable=False):
        data = path.read_bytes()
        if immutable and key in self.objects and self.objects[key] != data:
            raise ValueError('Published filename has different bytes: ' + key)
        self.objects[key] = data
        self.writes.append((key, immutable))


@unittest.skipUnless(shutil.which('gpg') and shutil.which('gpgv'), 'GnuPG required')
class RepoPublishTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-repository-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        gnupg = self.root / 'gnupg'
        gnupg.mkdir(mode=0o700)
        environment = patch.dict(os.environ, GNUPGHOME=str(gnupg))
        environment.start()
        self.addCleanup(environment.stop)
        subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Repository fixture <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        records = subprocess.check_output(['gpg', '--with-colons', '--list-secret-keys'], text=True)
        self.key = next(row.split(':')[9] for row in records.splitlines() if row.startswith('fpr:'))
        keys = self.root / 'config/keys'
        keys.mkdir(parents=True)
        for suffix, flags in (('gpg', []), ('asc', ['--armor'])):
            (keys / ('sunuefi-archive-keyring.' + suffix)).write_bytes(
                subprocess.check_output(['gpg', *flags, '--export', self.key]))
        root_patch = patch.object(repo, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        self.store = MemoryStore()

    def args(self, name, packages, generation):
        return SimpleNamespace(repo=name, packages=packages, key=self.key,
                               output=self.root / generation)

    def pacman(self, version, name='fixture'):
        path = self.root / (name + '-' + version + '-1-any.pkg.tar.gz')
        with tarfile.open(path, 'w:gz') as archive:
            data = (f'pkgname = {name}\npkgbase = {name}\npkgver = {version}-1\n'
                    'pkgdesc = Fixture\nurl = https://example.invalid\n'
                    'builddate = 1\npackager = Fixture\nsize = 8\n'
                    'arch = any\nlicense = MIT\n').encode()
            info = tarfile.TarInfo('.PKGINFO')
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
            data = b'fixture\n'
            info = tarfile.TarInfo('usr/share/' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        return path

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_pacman_retains_old_packages_and_reuses_signature(self):
        first, second = self.pacman('1'), self.pacman('2')
        repo.publish(self.args('sunuefi', [first], 'first'), self.store)
        prefix = 'pacman/sunuefi/os/aarch64/'
        signature = self.store.objects[prefix + first.name + '.sig']
        repo.publish(self.args('sunuefi', [second], 'second'), self.store)
        self.assertIn(prefix + first.name, self.store.objects)
        repo.publish(self.args('sunuefi', [first], 'third'), self.store)
        self.assertEqual(self.store.objects[prefix + first.name + '.sig'], signature)
        self.assertEqual(self.store.objects[prefix + 'sunuefi.db'],
                         self.store.objects[prefix + 'sunuefi.db.tar.zst'])
        immutable = [index for index, (_, fixed) in enumerate(self.store.writes) if fixed]
        self.assertLess(immutable[-1], len(self.store.writes) - 1)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_changed_package_cannot_overwrite_a_published_filename(self):
        package = self.pacman('1')
        repo.publish(self.args('sunuefi', [package], 'first'), self.store)
        before = dict(self.store.objects)
        package.write_bytes(package.read_bytes() + b'changed bytes')
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            repo.publish(self.args('sunuefi', [package], 'second'), self.store)
        self.assertEqual(before, self.store.objects)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_pacman_can_resume_after_incomplete_public_index_upload(self):
        base = 'sunuefi'
        prefix = 'pacman/sunuefi/os/aarch64/'
        repo.publish(self.args(base, [self.pacman('1')], 'first'), self.store)
        for name in ('sunuefi.db', 'sunuefi.db.tar.zst', 'sunuefi.files.tar.zst.sig'):
            self.store.objects[prefix + name] = b'interrupted upload'
        output = self.args(base, [self.pacman('2', 'device')], 'second')
        repo.publish(output, self.store)
        self.assertEqual(self.store.writes[-1][0], prefix + 'index.json')
        entries = repo.run('bsdtar', '-tf', output.output / 'sunuefi.db',
                           capture_output=True, text=True).stdout.splitlines()
        self.assertIn('fixture-1-1/desc', entries)
        self.assertIn('device-2-1/desc', entries)

    @unittest.skipUnless(all(shutil.which(name) for name in ('pacman', 'repo-add', 'bsdtar')),
                         'pacman tools required')
    def test_pacman_prefers_steamos_overlay_and_uses_base_for_other_packages(self):
        base, overlay = 'sunuefi', 'sunuefi-steamos'
        repo.publish(self.args(base, [self.pacman('2'), self.pacman('2', 'device')], base))
        repo.publish(self.args(overlay, [self.pacman('1')], overlay))
        dbpath = self.root / 'pacman-db'
        sync = dbpath / 'sync'
        sync.mkdir(parents=True)
        configs = self.root / 'pacman.conf'
        for name in (base, overlay):
            for suffix in ('.db', '.db.sig'):
                filename = name + suffix
                shutil.copyfile(self.root / name / filename, sync / filename)

        for names, expected in (
                ((base,), {f'{base}/fixture=2-1', f'{base}/device=2-1'}),
                ((overlay, base), {f'{overlay}/fixture=1-1', f'{base}/device=2-1'})):
            with self.subTest(repositories=names):
                configs.write_text('[options]\nArchitecture = aarch64\nSigLevel = Required\n'
                                   + ''.join(f'[{name}]\nServer = file://{self.root / name}\n'
                                             for name in names))
                result = repo.run('pacman', '--config', configs, '--dbpath', dbpath,
                                  '--gpgdir', self.root / 'gnupg', '-Sp',
                                  '--print-format', '%r/%n=%v', 'fixture', 'device',
                                  capture_output=True, text=True)
                self.assertEqual(set(result.stdout.splitlines()), expected)

    @unittest.skipUnless(all(shutil.which(name) for name in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')), 'APT tools required')
    def test_apt_inrelease_commit_and_old_versions_remain_fetchable(self):
        packages = []
        for version in ('1', '2'):
            if version == '2':
                # A failed upload may have written one of the fallback files.
                self.store.objects['apt/dists/sid/Release'] = b'incomplete next release'
            tree = self.root / ('package-' + version)
            control = tree / 'DEBIAN/control'
            control.parent.mkdir(parents=True)
            control.write_text(f'Package: fixture\nVersion: {version}\nArchitecture: all\n'
                               'Maintainer: Fixture <test@example.invalid>\nDescription: Fixture\n')
            package = self.root / ('fixture_' + version + '_all.deb')
            subprocess.run(['dpkg-deb', '--build', '--root-owner-group', tree, package], check=True)
            packages.append(package)
            repo.publish(self.args('apt', [package], 'generation-' + version), self.store)
        self.assertEqual(self.store.writes[-1][0], 'apt/dists/sid/InRelease')
        index = self.store.objects['apt/dists/sid/main/binary-arm64/Packages']
        self.assertIn(b'Version: 1', index)
        self.assertIn(b'Version: 2', index)
        for package in packages:
            self.assertIn('apt/pool/main/' + package.name, self.store.objects)
        self.assertTrue(any('/by-hash/SHA256/' in name for name in self.store.objects))

    def test_duplicate_or_wrong_format_inputs_fail_before_creating_output(self):
        package = self.pacman('1')
        for packages in ([package, package], [package]):
            args = self.args('apt', packages, 'bad')
            with self.assertRaises(ValueError):
                repo.publish(args)
            self.assertFalse(args.output.exists())

    def test_unrecognized_repository_is_rejected_before_signing(self):
        args = self.args('../outside', [self.pacman('1')], 'bad')
        with self.assertRaisesRegex(ValueError, 'Unknown package repository'):
            repo.publish(args, self.store)
        self.assertFalse(args.output.exists())
        self.assertFalse(self.store.writes)


if __name__ == '__main__':
    unittest.main()
