"""Sign real package indexes, merge releases and reject changed package bytes."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/repo'))
import products
import publish as repo


class KernelVersionTests(unittest.TestCase):
    def test_debian_revision_preserves_abi_epoch_check(self):
        previous = '7.3.3+1794995840.1794995840.aaaaaaaaaaaa-1'
        revision = '7.3.3+1794995840.1794995840.aaaaaaaaaaaa-2'
        another_abi = '7.3.3+1794995840.1794995840.bbbbbbbbbbbb-2'
        repo.check_kernel_epoch('linux-image-piano', revision, [previous])
        with self.assertRaisesRegex(ValueError, 'uses ABI epoch 1794995840'):
            repo.check_kernel_epoch('linux-image-piano', another_abi, [previous, revision])


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
        self.key = self.signing_key('Repository fixture <test@example.invalid>')
        keys = self.root / 'config/keys'
        keys.mkdir(parents=True)
        for suffix, flags in (('gpg', []), ('asc', ['--armor'])):
            (keys / ('sunuefi-archive-keyring.' + suffix)).write_bytes(
                subprocess.check_output(['gpg', *flags, '--export', self.key]))
        root_patch = patch.object(repo, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        self.store = MemoryStore()

    def signing_key(self, user):
        subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', user, 'ed25519', 'sign', '1d'],
                       check=True, capture_output=True)
        records = subprocess.check_output(
            ['gpg', '--batch', '--with-colons', '--list-secret-keys', user], text=True)
        return next(row.split(':')[9] for row in records.splitlines() if row.startswith('fpr:'))

    def args(self, name, packages, generation):
        return SimpleNamespace(repo=name, packages=packages, key=self.key,
                               output=self.root / generation)

    def debian(self, version, name='fixture'):
        tree = self.root / ('package-' + version)
        control = tree / 'DEBIAN/control'
        control.parent.mkdir(parents=True)
        control.write_text(f'Package: {name}\nVersion: {version}\nArchitecture: all\n'
                           'Maintainer: Fixture <test@example.invalid>\nDescription: Fixture\n')
        package = self.root / (name + '_' + version + '_all.deb')
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', tree, package],
                       check=True, capture_output=True)
        return package

    def pacman(self, version, name='fixture', *, replaces=(), conflicts=()):
        path = self.root / (name + '-' + version + '-1-any.pkg.tar.gz')
        with tarfile.open(path, 'w:gz') as archive:
            relations = ''.join(f'{field} = {target}\n'
                                for field, targets in (('replaces', replaces), ('conflict', conflicts))
                                for target in targets)
            data = (f'pkgname = {name}\npkgbase = {name}\npkgver = {version}-1\n'
                    'pkgdesc = Fixture\nurl = https://example.invalid\n'
                    'builddate = 1\npackager = Fixture\nsize = 8\n'
                    'arch = any\nlicense = MIT\n' + relations).encode()
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

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('repo-add', 'repo-remove', 'bsdtar')),
                         'pacman tools required')
    def test_pacman_retires_replaced_index_entries_without_removing_archives(self):
        base, overlay = 'sunuefi', 'sunuefi-steamos'
        prefix = f'pacman/{base}/os/aarch64/'
        overlay_prefix = f'pacman/{overlay}/os/aarch64/'
        plugin = self.pacman('2', 'pipewire-v4l2-source')
        lib = self.pacman('2', 'libpipewire', replaces=('pipewire-v4l2-source',),
                          conflicts=('pipewire-v4l2-source',))
        profiles = self.pacman('1', 'power-profiles-daemon')
        tuned_ppd = self.pacman('2', 'tuned-ppd', replaces=('power-profiles-daemon',),
                                conflicts=('power-profiles-daemon',))
        repo.publish(self.args(base, [plugin, profiles, tuned_ppd], 'before'), self.store)
        repo.publish(self.args(overlay, [plugin], 'overlay'), self.store)
        old_index = self.store.objects[prefix + base + '.db']
        old_signature = self.store.objects[prefix + plugin.name + '.sig']
        overlay_index = self.store.objects[overlay_prefix + overlay + '.db']

        with patch.object(products, 'verify_public'):
            products.publish_products({base: {lib: repo.sha(lib)}}, self.key,
                                      self.root / 'after', self.store)
            for suffix in ('.db', '.files'):
                path = self.root / 'after' / base / (base + suffix)
                entries, _ = repo.pacman_metadata(path)
                self.assertEqual(set(entries), {'libpipewire', 'power-profiles-daemon', 'tuned-ppd'})
            before_replay = list(self.store.writes)
            with self.assertRaisesRegex(ValueError, 'pipewire-v4l2-source .* missing from the index'):
                products.publish_products({base: {plugin: repo.sha(plugin)}}, self.key,
                                          self.root / 'replay', self.store)
            self.assertEqual(self.store.writes, before_replay)

        self.assertEqual(self.store.objects[prefix + plugin.name], plugin.read_bytes())
        self.assertEqual(self.store.objects[prefix + plugin.name + '.sig'], old_signature)
        self.assertEqual(self.store.objects[prefix + 'by-hash/SHA256/' +
                                           hashlib.sha256(old_index).hexdigest()], old_index)
        self.assertEqual(self.store.objects[overlay_prefix + overlay + '.db'], overlay_index)
        server = self.pacman('3', 'pipewire', replaces=('pipewire-v4l2-source',),
                             conflicts=('pipewire-v4l2-source',))
        scoped_base = self.store.objects[prefix + base + '.db']
        valve = self.args(overlay, [server], 'valve-native')
        repo.publish(valve, self.store)
        for suffix in ('.db', '.files'):
            entries, _ = repo.pacman_metadata(valve.output / (overlay + suffix))
            self.assertEqual(set(entries), {'pipewire'})
        self.assertEqual(self.store.objects[overlay_prefix + plugin.name], plugin.read_bytes())
        self.assertEqual(self.store.objects[prefix + base + '.db'], scoped_base)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('repo-add', 'repo-remove', 'bsdtar', 'vercmp')),
                         'pacman tools required')
    def test_pacman_only_retires_an_entry_matched_by_the_versioned_replacement(self):
        name = 'pipewire-v4l2-source'
        valve = self.pacman('1:1.6.8', name)
        arch = self.pacman('1:1.6.11', name)
        native = self.pacman('1:1.6.11', 'libpipewire', replaces=(name + '>=1:1.6.11-1',),
                             conflicts=(name,))
        repo.publish(self.args('sunuefi', [valve, native], 'before'), self.store)
        selected, retired = repo.pacman_metadata(self.root / 'before/sunuefi.db')
        self.assertEqual(selected[name][0], '1:1.6.8-1')
        self.assertFalse(retired)
        repo.publish(self.args('sunuefi', [arch], 'after'), self.store)
        selected, retired = repo.pacman_metadata(self.root / 'after/sunuefi.db')
        self.assertNotIn(name, selected)
        self.assertFalse(retired)
        self.assertIn('pacman/sunuefi/os/aarch64/' + arch.name, self.store.objects)

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('repo-add', 'repo-remove', 'bsdtar', 'vercmp')),
                         'pacman tools required')
    def test_shared_v4l2_withdrawal_is_verified_before_the_native_steamos_index(self):
        base, valve = 'sunuefi', 'sunuefi-steamos'
        legacy = 'pipewire-v4l2-source'
        common_prefix = f'pacman/{base}/os/aarch64/'
        valve_prefix = f'pacman/{valve}/os/aarch64/'
        old_arch = self.pacman('1:1.6.11', legacy)
        old_valve = self.pacman('1:1.6.8', legacy)
        kernel = self.pacman('1', 'linux-piano')
        repo.publish(self.args(base, [kernel, old_arch], 'old-arch'), self.store)
        repo.publish(self.args(valve, [old_valve], 'old-valve'), self.store)
        old_db = self.store.objects[common_prefix + base + '.db']
        old_steam_db = self.store.objects[valve_prefix + valve + '.db']
        libpipewire = self.pacman('1:1.6.11', 'libpipewire',
                                  replaces=(legacy + '>=1:1.6.11-1',), conflicts=(legacy,))
        pipewire = self.pacman('1:1.6.8', 'pipewire', replaces=(legacy,), conflicts=(legacy,))
        inputs = {base: {libpipewire: repo.sha(libpipewire)}, valve: {pipewire: repo.sha(pipewire)}}
        keyring = self.root / 'config/keys/sunuefi-archive-keyring.gpg'
        checked = []
        withdrawal_folder = self.root / 'resumed' / 'sunuefi-v4l2-withdrawal'

        def verify_indexes(_, repository, directory):
            prefix = f'pacman/{repository}/os/aarch64/'
            database = directory / (repository + '.db')
            repo.verify(database, directory / (repository + '.db.sig'), keyring)
            self.assertEqual(self.store.objects[prefix + database.name], database.read_bytes())
            indexed, _ = repo.pacman_metadata(database)
            self.assertNotIn(legacy, indexed)
            checked.append((repository, sorted(indexed)))
            if list(indexed) == ['linux-piano']:
                self.assertEqual(self.store.objects[valve_prefix + valve + '.db'], old_steam_db)
            if repository == valve:
                self.assertEqual(self.store.objects[common_prefix + base + '.db'],
                                 (withdrawal_folder / (base + '.db')).read_bytes())

        # The upload has finished but the public domain has not caught up.
        with patch.object(products, 'verify_public', side_effect=ValueError('withdrawal not visible')):
            with self.assertRaisesRegex(ValueError, 'withdrawal not visible'):
                products.publish_products(inputs, self.key, self.root / 'interrupted', self.store)
        self.assertEqual(self.store.objects[valve_prefix + valve + '.db'], old_steam_db)
        self.assertNotIn(common_prefix + libpipewire.name, self.store.objects)
        intermediate = self.store.objects[common_prefix + base + '.db']
        self.assertEqual(intermediate, (self.root / 'interrupted' / 'sunuefi-v4l2-withdrawal'
                                        / (base + '.db')).read_bytes())
        with patch.object(products, 'verify_public', side_effect=verify_indexes):
            products.publish_products(inputs, self.key, self.root / 'resumed', self.store)
        self.assertEqual(checked, [(base, ['linux-piano']), (valve, ['pipewire']),
                                   (base, ['libpipewire', 'linux-piano'])])
        self.assertEqual(self.store.objects[common_prefix + old_arch.name], old_arch.read_bytes())
        self.assertEqual(self.store.objects[common_prefix + 'by-hash/SHA256/' +
                                            hashlib.sha256(old_db).hexdigest()], old_db)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_product_rejects_older_pacman_version_before_updating_any_index(self):
        base, overlay = 'sunuefi', 'sunuefi-steamos'
        old, newer = self.pacman('1'), self.pacman('2')
        common = self.pacman('1', 'device')
        with patch.object(products, 'verify_public') as verify_public:
            for package, output in ((old, 'first'), (newer, 'second')):
                products.publish_products({overlay: {package: repo.sha(package)}},
                                          self.key, self.root / output, self.store)
            before = self.store.objects.copy()
            writes = self.store.writes.copy()
            verify_public.reset_mock()
            batches = {base: {common: repo.sha(common)}, overlay: {old: repo.sha(old)}}
            with self.assertRaisesRegex(ValueError, r'fixture 1-1 .*index selects 2-1'):
                products.publish_products(batches, self.key, self.root / 'stale', self.store)
            verify_public.assert_not_called()
        self.assertEqual(self.store.objects, before)
        self.assertEqual(self.store.writes, writes)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_shared_sensors_wait_for_the_signed_steamos_index(self):
        base, overlay = 'sunuefi', 'sunuefi-steamos'
        base_prefix = f'pacman/{base}/os/aarch64/'
        provider = self.pacman('3.9', 'iio-sensor-proxy')
        sensors = self.pacman('5', 'piano-sensors')
        previous = self.pacman('4', 'piano-sensors')
        repo.publish(self.args(base, [previous], 'previous'), self.store)
        before = {key: value for key, value in self.store.objects.items()
                  if key.startswith(base_prefix)}
        batches = {overlay: {provider: repo.sha(provider)}, base: {sensors: repo.sha(sensors)}}

        def verify_overlay(_, repository, directory):
            self.assertEqual(repository, overlay)
            index = directory / f'{overlay}.db'
            repo.verify(index, directory / f'{overlay}.db.sig',
                        self.root / 'config/keys/sunuefi-archive-keyring.gpg')
            self.assertEqual(self.store.objects[f'pacman/{overlay}/os/aarch64/{overlay}.db'],
                             index.read_bytes())
            raise ValueError('The client-facing index still serves the previous generation')

        with patch.object(products, 'verify_public', side_effect=verify_overlay) as verify_public:
            with self.assertRaisesRegex(ValueError, 'still serves the previous generation'):
                products.publish_products(batches, self.key, self.root / 'deferred', self.store)
            verify_public.assert_called_once()
        after = {key: value for key, value in self.store.objects.items()
                 if key.startswith(base_prefix)}
        self.assertEqual(after, before)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_product_accepts_republishing_the_same_pacman_generation(self):
        package = self.pacman('2')
        digest = repo.sha(package)
        batches = {'sunuefi': {package: digest}}
        prefix = 'pacman/sunuefi/os/aarch64/'
        with patch.object(products, 'verify_public'):
            products.publish_products(batches, self.key, self.root / 'first', self.store)
            signature = self.store.objects[prefix + package.name + '.sig']
            products.publish_products(batches, self.key, self.root / 'again', self.store)
        self.assertEqual(self.store.objects[prefix + package.name + '.sig'], signature)
        entry = repo.run('bsdtar', '-xOf', self.root / 'again/sunuefi/sunuefi.db',
                         'fixture-2-1/desc', capture_output=True, text=True).stdout
        self.assertIn('%NAME%\nfixture\n', entry)
        self.assertIn('%VERSION%\n2-1\n', entry)
        self.assertIn(f'%SHA256SUM%\n{digest}\n', entry)

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                             ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive', 'repo-add', 'bsdtar')),
                         'native repository tools required')
    def test_product_rejects_different_kernel_abis_with_the_same_epoch(self):
        formats = {
            'apt': lambda version: self.debian(version + '-1', 'linux-image-piano'),
            'sunuefi': lambda version: self.pacman(version, 'linux-piano'),
        }
        versions = {
            'first': '7.3.3+1794995840.1794995840.aaaaaaaaaaaa',
            'metadata': '7.3.3+1794995840.1794996428.aaaaaaaaaaaa',
            'changed-abi': '7.3.3+1794995840.1794996428.bbbbbbbbbbbb',
            'new-abi': '7.3.3+1794997000.1794997000.bbbbbbbbbbbb',
        }
        with patch.object(products, 'verify_public'):
            for repository, package in formats.items():
                with self.subTest(repository=repository):
                    for stage, version in versions.items():
                        archive = package(version)
                        batches = {repository: {archive: repo.sha(archive)}}
                        output = self.root / f'{repository}-{stage}'
                        if stage == 'changed-abi':
                            before = list(self.store.writes)
                            with self.assertRaisesRegex(ValueError, 'uses ABI epoch 1794995840'):
                                products.publish_products(batches, self.key, output, self.store)
                            self.assertEqual(self.store.writes, before)
                        else:
                            products.publish_products(batches, self.key, output, self.store)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_pacman_selection_requires_the_sealed_filename_and_digest(self):
        package = self.pacman('2')
        output = self.args('sunuefi', [package], 'hash')
        repo.publish(output)
        for filename, digest in ((package.name, '0' * 64), ('different.pkg.tar.zst', repo.sha(package))):
            with self.subTest(filename=filename), self.assertRaisesRegex(
                    ValueError, 'does not match the sealed filename and SHA-256'):
                repo.check_pacman_selection(output.output / 'sunuefi.db',
                                            {'fixture': ('2-1', filename, digest)})

    def test_product_rechecks_package_bytes_before_indexing(self):
        package = self.pacman('2')
        batches = {'sunuefi': {package: repo.sha(package)}}
        package.write_bytes(b'changed after verifying the manifest')
        with self.assertRaisesRegex(ValueError, 'Package changed while preparing publication'):
            products.publish_products(batches, self.key, self.root / 'changed', self.store)
        self.assertFalse(self.store.writes)

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
                for name in list(self.store.objects):
                    if '/by-hash/SHA512/' in name:
                        del self.store.objects[name]
            package = self.debian(version)
            packages.append(package)
            repo.publish(self.args('apt', [package], 'generation-' + version), self.store)
        self.assertEqual(self.store.writes[-1][0], 'apt/dists/sid/InRelease')
        index = self.store.objects['apt/dists/sid/main/binary-arm64/Packages']
        self.assertIn(b'Version: 1', index)
        self.assertIn(b'Version: 2', index)
        for package in packages:
            self.assertIn('apt/pool/main/' + package.name, self.store.objects)
        path = 'apt/dists/sid/main/binary-arm64/'
        for algorithm in ('SHA512', 'SHA256'):
            for name in ('Packages', 'Packages.gz'):
                content = self.store.objects[path + name]
                digest = hashlib.new(algorithm.lower(), content).hexdigest()
                key = path + 'by-hash/' + algorithm + '/' + digest
                self.assertEqual(self.store.objects[key], content)
                self.assertIn((key, True), self.store.writes)

    def test_apt_accepts_last_signed_index_without_a_final_newline(self):
        release = self.root / 'Release'
        digest = 'a' * 128
        indexes = ('main/binary-arm64/Packages', 'main/binary-arm64/Packages.gz')
        release.write_text('SHA512:\n' + '\n'.join(f' {digest} 17 {name}' for name in indexes))
        self.assertEqual(repo.apt_index_hashes(release, 'SHA512'),
                         {name: (17, digest) for name in indexes})

    def assert_no_upload_for_untrusted_signer(self, args):
        previous_objects = self.store.objects.copy()
        previous_writes = self.store.writes.copy()
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            repo.publish(args, self.store)
        self.assertEqual(failure.exception.cmd[0], 'gpgv')
        self.assertEqual(self.store.objects, previous_objects)
        self.assertEqual(self.store.writes, previous_writes)

    @unittest.skipUnless(all(shutil.which(name) for name in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')), 'APT tools required')
    def test_apt_rejects_untrusted_release_signer_before_any_upload(self):
        repo.publish(self.args('apt', [self.debian('1')], 'first'), self.store)
        package = self.debian('2')
        key = self.signing_key('Other archive <other@example.invalid>')
        args = self.args('apt', [package], 'second')
        args.key = key
        self.assert_no_upload_for_untrusted_signer(args)

        local = self.args('apt', [package], 'local')
        local.key = key
        self.assertFalse(repo.publish(local)['published'])

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'), 'pacman tools required')
    def test_pacman_rejects_untrusted_index_signer_even_with_existing_package_signature(self):
        package = self.pacman('1')
        repo.publish(self.args('sunuefi', [package], 'first'), self.store)
        # The valid signature on this previously published package is reused.
        args = self.args('sunuefi', [package], 'second')
        args.key = self.signing_key('Other archive <other@example.invalid>')
        self.assert_no_upload_for_untrusted_signer(args)

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
