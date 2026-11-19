"""Check the shared official Mesa source and native recipe generation."""
from contextlib import contextmanager, redirect_stdout
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from build import mesa


@contextmanager
def archive_server(responses):
    calls = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            index = len(calls)
            calls.append(self.path)
            status, body, length = responses[min(index, len(responses) - 1)]
            self.send_response(status)
            self.send_header('Content-Length', length)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.01})
        worker.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}/source.tar.gz', calls
        finally:
            server.shutdown()
            worker.join()


class MesaTests(unittest.TestCase):
    def test_official_pin_is_shared_and_has_no_chip_backport(self):
        data = mesa.policy()
        self.assertEqual(data['source']['version'], '27.0.0-devel')
        self.assertEqual(len(data['source']['commit']), 40)
        self.assertTrue(data['source']['url'].startswith(data['source']['repository'][:-4]))
        self.assertFalse((mesa.ROOT / 'patches/mesa/26.2.6/series.json').exists())

    def test_recipe_retains_all_other_drivers_and_supported_layers(self):
        original = ('GALLIUM_DEFAULTS = softpipe\nGALLIUM_DRIVERS += freedreno llvmpipe softpipe\n'
                    'VULKAN_LAYERS += anti-lag device-select intel-nullhw overlay screenshot\n')
        changed = mesa.debian_rules(original)
        self.assertIn('GALLIUM_DRIVERS += freedreno llvmpipe softpipe', changed)
        self.assertNotIn('GALLIUM_DEFAULTS = softpipe', changed)
        self.assertIn('vram-report-limit', changed)
        self.assertNotIn('anti-lag', changed)

    def test_official_source_integrity_refuses_local_edits(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / 'meson.build'
            source.write_text('official source\n')
            proof = {'official_source': mesa.policy()['source'],
                     'files': {'meson.build': mesa.package_build.sha(source)}}
            mesa.verify_official_source(work, proof)
            source.write_text('modified source\n')
            with self.assertRaisesRegex(ValueError, 'Official Mesa source changed'):
                mesa.verify_official_source(work, proof)

    def test_subproject_retries_http_500_and_reuses_verified_archive(self):
        archive = b'fixed source bytes'
        responses = [(500, b'', 0), (200, archive, len(archive))]
        with (archive_server(responses) as (url, calls),
              tempfile.TemporaryDirectory() as temporary,
              patch.object(mesa.time, 'sleep') as wait,
              redirect_stdout(io.StringIO()) as output):
            root = Path(temporary)
            recipe = root / 'recipe'
            recipe.mkdir()
            pin = {'url': url, 'sha256': hashlib.sha256(archive).hexdigest()}
            with patch.object(mesa, 'ROOT', root):
                staged = mesa.subproject_archive(pin, recipe)
                cached = mesa.subproject_archive(pin)
                self.assertEqual(staged.read_bytes(), archive)
                self.assertEqual(cached.read_bytes(), archive)
                self.assertEqual(list(cached.parent.iterdir()), [cached])
                cached.write_bytes(b'corrupt cache')
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    mesa.subproject_archive(pin)
            self.assertEqual(calls, ['/source.tar.gz'] * 2)
            wait.assert_called_once_with(15)
            self.assertIn(url, output.getvalue())

    def test_download_retries_truncated_http_body(self):
        archive = b'fixed source bytes'
        responses = [(200, archive[:-1], len(archive)), (200, archive, len(archive))]
        with (archive_server(responses) as (url, calls),
              tempfile.TemporaryDirectory() as temporary,
              patch.object(mesa.time, 'sleep') as wait,
              redirect_stdout(io.StringIO())):
            destination = Path(temporary) / 'archive'
            mesa.download(url, destination, hashlib.sha256(archive).hexdigest(), len(archive))
            self.assertEqual(destination.read_bytes(), archive)
            self.assertEqual(calls, ['/source.tar.gz'] * 2)
            wait.assert_called_once_with(15)

    def test_download_attempts_are_bounded_and_permanent_errors_are_terminal(self):
        for status, requests in ((500, 3), (403, 1), (404, 1)):
            with (self.subTest(status=status),
                  archive_server([(status, b'', 0)]) as (url, calls),
                  tempfile.TemporaryDirectory() as temporary,
                  patch.object(mesa.time, 'sleep') as wait,
                  redirect_stdout(io.StringIO())):
                destination = Path(temporary) / 'archive'
                with self.assertRaisesRegex(OSError, f'HTTP Error {status}') as caught:
                    mesa.download(url, destination, 'a' * 64)
                self.assertIn(url, str(caught.exception))
                self.assertEqual(calls, ['/source.tar.gz'] * requests)
                self.assertEqual(wait.call_count, requests - 1)
                self.assertEqual(list(destination.parent.iterdir()), [])

    def test_download_rejects_wrong_checksum_or_size_without_retry(self):
        archive = b'fixed source bytes'
        digest = hashlib.sha256(archive).hexdigest()
        for wanted_hash, wanted_size in (('a' * 64, len(archive)), (digest, len(archive) + 1)):
            with (self.subTest(digest=wanted_hash, size=wanted_size),
                  archive_server([(200, archive, len(archive))]) as (url, calls),
                  tempfile.TemporaryDirectory() as temporary,
                  patch.object(mesa.time, 'sleep') as wait):
                destination = Path(temporary) / 'archive'
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    mesa.download(url, destination, wanted_hash, wanted_size)
                self.assertEqual(calls, ['/source.tar.gz'])
                self.assertEqual(list(destination.parent.iterdir()), [])
                wait.assert_not_called()


if __name__ == '__main__':
    unittest.main()
