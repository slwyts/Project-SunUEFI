"""Exercise the signed public URLs using a local HTTP server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/repo'))
import publish
import verify_public as public


@unittest.skipUnless(all(shutil.which(tool) for tool in ('curl', 'gpg', 'gpgv')),
                     'curl and GnuPG required')
class PublicRepositoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='sunuefi-public-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        gnupg = self.root / 'gnupg'
        gnupg.mkdir(mode=0o700)
        self.enterContext(patch.dict(os.environ, GNUPGHOME=str(gnupg),
                                     NO_PROXY='127.0.0.1', no_proxy='127.0.0.1'))
        self.enterContext(patch.object(publish, 'ROOT', self.root))
        self.enterContext(patch.object(public, 'ROOT', self.root))
        self.enterContext(patch.object(public, 'RETRY_DELAYS', (0, 0)))
        subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                        '--quick-generate-key', 'Archive fixture <test@example.invalid>',
                        'ed25519', 'sign', '1d'], check=True, capture_output=True)
        keys = subprocess.check_output(['gpg', '--with-colons', '--list-secret-keys'], text=True)
        self.fingerprint = next(line.split(':')[9] for line in keys.splitlines()
                                if line.startswith('fpr:'))
        keyring = self.root / 'config/keys/sunuefi-archive-keyring.gpg'
        keyring.parent.mkdir(parents=True)
        keyring.write_bytes(subprocess.check_output(['gpg', '--export', self.fingerprint]))
        self.requests = []
        self.objects = {}
        requests, objects = self.requests, self.objects

        class ServerHandler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                body = objects.get(self.path)
                if isinstance(body, list):
                    body = body.pop(0) if len(body) > 1 else body[0]
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), ServerHandler)
        self.url = f'http://127.0.0.1:{server.server_port}'
        self.addCleanup(server.server_close)
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.01},
                                  daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(server.shutdown)

    def generation(self, repository, version):
        if repository == 'apt':
            package = self.root / f'fixture_{version}_all.deb'
            tree = self.root / ('package-' + version)
            control = tree / 'DEBIAN/control'
            control.parent.mkdir(parents=True)
            control.write_text(f'Package: fixture\nVersion: {version}\nArchitecture: all\n'
                               'Maintainer: Fixture <test@example.invalid>\nDescription: Fixture\n')
            subprocess.run(['dpkg-deb', '--build', '--root-owner-group', tree, package],
                           check=True, capture_output=True)
            prefix = '/apt/'
        else:
            package = self.root / f'fixture-{version}-1-any.pkg.tar.gz'
            with tarfile.open(package, 'w:gz') as archive:
                metadata = (f'pkgname = fixture\npkgbase = fixture\npkgver = {version}-1\n'
                            'pkgdesc = Fixture\nurl = https://example.invalid\nbuilddate = 1\n'
                            'packager = Test\nsize = 7\narch = any\nlicense = MIT\n').encode()
                for name, data in (('.PKGINFO', metadata), ('usr/share/fixture', b'fixture')):
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
            prefix = f'/pacman/{repository}/os/aarch64/'
        output = self.root / (repository + '-' + version)
        publish.publish(SimpleNamespace(repo=repository, key=self.fingerprint, output=output,
                                        packages=[package]))
        self.objects.update({prefix + path.relative_to(output).as_posix(): path.read_bytes()
                             for path in output.rglob('*') if path.is_file()})
        return output

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')),
                         'APT packaging tools required')
    def test_apt_waits_for_new_inrelease_then_checks_both_by_hash_indexes(self):
        self.generation('apt', '1')
        path = '/apt/dists/sid/InRelease'
        old = self.objects[path]
        output = self.generation('apt', '2')
        self.objects[path] = [old, None, self.objects[path]]

        public.verify_public(self.url, 'apt', output)

        by_hash = '/apt/dists/sid/main/binary-arm64/by-hash/'
        binary = output / 'dists/sid/main/binary-arm64'
        self.assertEqual(self.requests, [path] * 3 + [
            by_hash + algorithm + '/' + publish.sha(binary / name, algorithm.lower())
            for algorithm in ('SHA512', 'SHA256') for name in ('Packages', 'Packages.gz')
        ])

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')),
                         'APT packaging tools required')
    def test_apt_rejects_an_older_but_valid_signed_index(self):
        old = self.generation('apt', '1')
        public.verify_public(self.url, 'apt', old)
        path = '/apt/dists/sid/InRelease'
        previous = self.objects[path]
        output = self.generation('apt', '2')
        self.objects[path] = previous
        self.requests.clear()

        with self.assertRaisesRegex(public.PublicIndexError, 'after 3 attempts: .*InRelease'):
            public.verify_public(self.url, 'apt', output)
        self.assertEqual(self.requests, [path] * 3)

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')),
                         'APT packaging tools required')
    def test_apt_rejects_corruption_of_an_index_named_by_the_signature(self):
        output = self.generation('apt', '1')
        digest = publish.sha(output / 'dists/sid/main/binary-arm64/Packages.gz', 'sha512')
        key = '/apt/dists/sid/main/binary-arm64/by-hash/SHA512/' + digest
        body = self.objects[key]
        self.objects[key] = bytes([body[0] ^ 1]) + body[1:]

        with self.assertRaisesRegex(public.PublicIndexError, 'by-hash/SHA512/.*bytes do not match'):
            public.verify_public(self.url, 'apt', output)

    @unittest.skipUnless(all(shutil.which(tool) for tool in
                            ('dpkg-deb', 'dpkg-scanpackages', 'apt-ftparchive')),
                         'APT packaging tools required')
    def test_missing_sha512_is_not_hidden_by_live_mutable_index(self):
        output = self.generation('apt', '1')
        digest = publish.sha(output / 'dists/sid/main/binary-arm64/Packages.gz', 'sha512')
        del self.objects['/apt/dists/sid/main/binary-arm64/by-hash/SHA512/' + digest]

        with self.assertRaisesRegex(public.PublicIndexError, 'by-hash/SHA512/.*404'):
            public.verify_public(self.url, 'apt', output)
        self.assertNotIn('/apt/dists/sid/main/binary-arm64/Packages.gz', self.requests)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'),
                         'pacman packaging tools required')
    def test_pacman_client_aliases_all_match_the_new_signed_generation(self):
        name = 'sunuefi-steamos'
        output = self.generation(name, '1')
        public.verify_public(self.url, name, output)
        prefix = f'/pacman/{name}/os/aarch64/{name}'
        self.assertEqual(self.requests, [prefix + ext for ext in
                                        ('.db', '.db.sig', '.files', '.files.sig')])

        # A bad local signature and an identical uploaded signature must still fail.
        bad = b'not a signature'
        (output / (name + '.files.sig')).write_bytes(bad)
        self.objects[prefix + '.files.sig'] = bad
        with self.assertRaisesRegex(public.PublicIndexError, 'files: signature verification failed'):
            public.verify_public(self.url, name, output)

    @unittest.skipUnless(shutil.which('repo-add') and shutil.which('bsdtar'),
                         'pacman packaging tools required')
    def test_pacman_rechecks_a_database_and_signature_from_different_generations(self):
        name = 'sunuefi'
        prefix = f'/pacman/{name}/os/aarch64/{name}'
        signature = prefix + '.db.sig'
        self.generation(name, '1')
        old = self.objects[signature]
        output = self.generation(name, '2')
        self.objects[signature] = [old, self.objects[signature]]

        public.verify_public(self.url, name, output)

        self.assertEqual(self.requests,
                         [prefix + ext for ext in
                          ('.db', '.db.sig', '.db', '.db.sig', '.files', '.files.sig')])

    def test_http_timeout_is_bounded(self):
        output = self.root / 'output/dists/sid'
        output.mkdir(parents=True)
        (output / 'InRelease').write_bytes(b'expected index')
        with socket.socket() as listener, patch.object(public, 'HTTP_TIMEOUT', 0.05):
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            url = f'http://127.0.0.1:{listener.getsockname()[1]}'
            with self.assertRaisesRegex(public.PublicIndexError, 'after 3 attempts: .*timed out'):
                public.verify_public(url, 'apt', output.parents[1])


if __name__ == '__main__':
    unittest.main()
