"""Build teardown removes dead IPC, not keys or unrelated special files."""
from pathlib import Path
import socket
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from build_chroot import remove_pacman_sockets


class PacmanSocketCleanupTests(unittest.TestCase):
    def test_dead_socket_removed_and_keyring_preserved(self):
        # /tmp keeps the socket path under the AF_UNIX length limit
        with tempfile.TemporaryDirectory(prefix='piano-keyring-', dir='/tmp') as folder:
            root = Path(folder)
            keyring = root / 'etc/pacman.d/gnupg'
            keyring.mkdir(parents=True)
            key = keyring / 'pubring.kbx'
            key.write_bytes(b'KEYRING FIXTURE')
            regular = keyring / 'S.regular'
            regular.write_bytes(b'NOT A SOCKET')
            endpoint = keyring / 'S.gpg-agent'
            with socket.socket(socket.AF_UNIX) as sock:
                sock.bind(str(endpoint))
            remove_pacman_sockets(root)
            self.assertFalse(endpoint.exists())
            self.assertEqual(key.read_bytes(), b'KEYRING FIXTURE')
            self.assertEqual(regular.read_bytes(), b'NOT A SOCKET')
