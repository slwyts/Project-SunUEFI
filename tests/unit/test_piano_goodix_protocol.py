"""Goodix request layouts and the TA file service, without a device

Offsets come from OS4.0.4.0.XPYCNXM libgf_hal.so (SHA256 36a176de...); the
source function and address are named next to each value
"""
import ctypes
import errno
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'linux/userspace/fingerprint/piano-goodix-protocol.c'

PREFIX = b'/data/vendor_de/0/fpdata/'
SERVICE_BYTES = 504 * 1024
PATH_BYTES = 256

# (command, size, {offset: u32 value})
AUTHENTICATE = (1010, 0x80, {0x6c: 1000, 0x78: 0xffffffff})  # gf_hal_common_authenticate 0x21a18
IRQ = (1016, 0xd134, {0x98: 1, 0xa8: 0x200})                  # hal_milan_irq 0x41390
IRQ_TOKEN_TYPE = (0x2c5, bytes([0, 0, 0, 2]))                  # big-endian HW_AUTH_FINGERPRINT
FINISH = (1086, 0x148)                                         # hal_milan_authenticate_finish 0x428d8
GROUP = (1014, 0x270, 0x6c, 0x70, 0x200)                       # gf_hal_common_set_active_group 0x2208c

# irq_image_enroll_or_authenticate 0x42308: (operation, GF status) -> result
NONE, MATCH, NO_MATCH, RETRY, PARTIAL, DIRTY, TOO_FAST, TOO_SLOW, PALM, ERROR = range(10)
RESULTS = [
    (1, 0, MATCH), (2, 0, MATCH), (0, 0, NONE), (3, 0, NONE),
    (2, 1006, NO_MATCH), (2, 1119, NO_MATCH), (2, 1064, RETRY),
    (2, 1011, PARTIAL), (2, 1060, PARTIAL), (2, 1012, DIRTY), (2, 1101, DIRTY),
    (2, 1104, DIRTY), (2, 1094, TOO_FAST), (2, 1117, TOO_SLOW), (2, 1110, PALM),
    (2, 1057, ERROR),
]


LIBRARY = {}


class Counts(ctypes.Structure):
    _fields_ = [('reads', ctypes.c_uint), ('writes', ctypes.c_uint),
                ('removes', ctypes.c_uint), ('renames', ctypes.c_uint),
                ('refused', ctypes.c_uint), ('refused_command', ctypes.c_uint32),
                ('refused_path', ctypes.c_char * 257)]


def setUpModule():
    compiler = shutil.which('cc')
    if not compiler:
        raise unittest.SkipTest('Host C compiler unavailable')
    work = tempfile.TemporaryDirectory()
    library = Path(work.name) / 'goodix-protocol.so'
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-fPIC', '-o', library, SOURCE], check=True)
    lib = ctypes.CDLL(str(library))
    lib.goodix_file_service.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t,
                                        ctypes.POINTER(Counts)]
    lib.goodix_set_active_group_request.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p]
    lib.goodix_authenticate_request.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.goodix_authenticate_result.argtypes = [ctypes.c_uint32, ctypes.c_int]
    LIBRARY.update(work=work, lib=lib)


def tearDownModule():
    if 'work' in LIBRARY:
        LIBRARY['work'].cleanup()


class GoodixProtocolTest(unittest.TestCase):
    def setUp(self):
        self.lib = LIBRARY['lib']

    def buffer(self, size):
        return (ctypes.c_uint8 * size)()

    @staticmethod
    def u32(data, offset):
        return struct.unpack_from('<I', bytes(data), offset)[0]

    def assert_only(self, data, words, extra=()):
        expected = bytearray(len(data))
        for offset, value in words.items():
            struct.pack_into('<I', expected, offset, value)
        for offset, raw in extra:
            expected[offset:offset + len(raw)] = raw
        self.assertEqual(bytes(data), bytes(expected))

    def test_authenticate_request(self):
        _, size, words = AUTHENTICATE
        data = self.buffer(size)
        self.lib.goodix_authenticate_request(data, 1000)
        self.assert_only(data, words)

    def test_irq_request_and_retry(self):
        _, size, words = IRQ
        data = self.buffer(size)
        ctypes.memset(data, 0xa5, size)
        self.lib.goodix_irq_request(data)
        self.assert_only(data, words, [IRQ_TOKEN_TYPE])
        struct.pack_into('<II', data, 0x98, 7, 9)
        self.lib.goodix_irq_capture_again(data)
        self.assertEqual(struct.unpack_from('<II', bytes(data), 0x98), (2, 0))

    def test_authenticate_finish_request(self):
        irq = self.buffer(IRQ[1])
        struct.pack_into('<II', irq, 0x6c, 0x80, 2)   # IRQ type, operation
        struct.pack_into('<II', irq, 0x80, 1000, 77)  # group, finger
        data = self.buffer(FINISH[1])
        self.lib.goodix_authenticate_finish_request(data, irq)
        self.assert_only(data, {0x70: 2, 0x74: 0x80, 0x78: 1000, 0x7c: 77})

    def test_set_active_group_request(self):
        _, size, group_at, path_at, path_limit = GROUP
        data = self.buffer(size)
        self.assertEqual(self.lib.goodix_set_active_group_request(
            data, 1000, b'/var/lib/piano-goodix'), 0)
        self.assertEqual(self.u32(data, group_at), 1000)
        self.assertEqual(bytes(data[path_at:path_at + 22]), b'/var/lib/piano-goodix\0')
        self.assertEqual(self.lib.goodix_set_active_group_request(
            data, 1, b'x' * path_limit), -errno.ENAMETOOLONG)

    def test_authentication_results(self):
        for operation, status, result in RESULTS:
            with self.subTest(operation=operation, status=status):
                self.assertEqual(self.lib.goodix_authenticate_result(operation, status), result)


class GoodixFileServiceTest(unittest.TestCase):
    """Legacy GPFS commands 0..3 and 12 (minkipc gpfs_msg.h field layouts)"""

    def setUp(self):
        self.lib = LIBRARY['lib']
        self.state = tempfile.TemporaryDirectory()
        self.root = Path(self.state.name)
        self.fd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
        self.data = (ctypes.c_uint8 * SERVICE_BYTES)()
        self.counts = Counts()

    def tearDown(self):
        os.close(self.fd)
        self.state.cleanup()

    def request(self, command, *fields):
        ctypes.memset(self.data, 0x5a, SERVICE_BYTES)
        offset = 0
        struct.pack_into('<I', self.data, offset, command)
        offset = 4
        for field in fields:
            if isinstance(field, bytes):
                raw = field.ljust(PATH_BYTES, b'\0') if len(field) < PATH_BYTES else field
                self.data[offset:offset + len(raw)] = raw
                offset += len(raw)
            else:
                struct.pack_into('<i' if field < 0 else '<I', self.data, offset, field)
                offset += 4
        return offset

    def serve(self):
        self.assertEqual(self.lib.goodix_file_service(self.fd, self.data, SERVICE_BYTES,
                                                      ctypes.byref(self.counts)), 0)
        return struct.unpack_from('<iI', bytes(self.data[4:12]))

    def write(self, name, payload, offset=0, backup=0):
        start = self.request(1, PREFIX + name, offset, len(payload), backup)
        self.data[start:start + len(payload)] = payload
        return self.serve()

    def read(self, name, offset=0, count=4096):
        self.request(0, PREFIX + name, offset, count)
        error, done = self.serve()
        return error, bytes(self.data[12:12 + done])

    def test_write_read_rename_remove(self):
        self.assertEqual(self.write(b'finger_1000_0.so', b'ciphertext'), (0, 10))
        self.assertEqual((self.root / 'finger_1000_0.so').stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.read(b'finger_1000_0.so'), (0, b'ciphertext'))
        self.assertEqual(self.read(b'finger_1000_0.so', offset=6, count=2), (0, b'te'))
        self.assertEqual(self.read(b'finger_1000_0.so', offset=20), (0, b''))
        self.request(3, PREFIX + b'finger_1000_0.so', PREFIX + b'finger_1000_1.so')
        self.assertEqual(self.serve()[0], 0)
        self.assertEqual(self.read(b'finger_1000_1.so'), (0, b'ciphertext'))
        self.request(2, PREFIX + b'finger_1000_1.so')
        self.assertEqual(self.serve()[0], 0)
        self.assertEqual(self.read(b'finger_1000_1.so')[0], errno.ENOENT)
        self.assertEqual(sorted(os.listdir(self.root)), [])
        self.assertEqual((self.counts.reads, self.counts.writes, self.counts.removes,
                          self.counts.renames, self.counts.refused), (5, 1, 1, 1, 0))

    def test_write_keeps_backup_and_offset(self):
        self.write(b'auth_token_1000.so', b'first')
        self.assertEqual(self.write(b'auth_token_1000.so', b'X', offset=1, backup=1), (0, 1))
        self.assertEqual((self.root / 'auth_token_1000.so.bak').read_bytes(), b'first')
        self.assertEqual((self.root / 'auth_token_1000.so').read_bytes(), b'fXrst')

    def test_refuses_paths_outside_the_state_directory(self):
        for path in [b'/persist/data/ifaa_fplist', b'/data/vendor_de/10/fpdata/finger_0_0.so',
                     PREFIX + b'../finger_0_0.so', PREFIX + b'sub/finger_0_0.so',
                     PREFIX + b'.hidden', PREFIX, PREFIX + b'a' * 97, b'finger_0_0.so']:
            with self.subTest(path=path):
                self.request(1, path, 0, 1, 0)
                self.assertEqual(self.serve(), (errno.EACCES, 0))
        self.request(3, PREFIX + b'a.so', b'/persist/a.so')
        self.assertEqual(self.serve()[0], errno.EACCES)
        self.assertEqual(os.listdir(self.root), [])
        self.assertEqual((self.counts.refused, self.counts.refused_command,
                          self.counts.refused_path), (9, 3, b'/persist/a.so'))
        self.assertEqual(self.counts.writes + self.counts.renames, 0)

    def test_does_not_follow_symlinks(self):
        target = self.root.parent / (self.root.name + '-outside')
        target.write_bytes(b'outside')
        self.addCleanup(target.unlink)
        os.symlink(target, self.root / 'link.so')
        self.assertEqual(self.read(b'link.so')[0], errno.ELOOP)
        self.assertEqual(self.write(b'link.so', b'x')[0], errno.ELOOP)
        self.assertEqual(target.read_bytes(), b'outside')

    def test_version_and_unknown_commands(self):
        self.request(12)
        self.assertEqual(self.lib.goodix_file_service(self.fd, self.data, SERVICE_BYTES,
                                                      ctypes.byref(self.counts)), 0)
        self.assertEqual(struct.unpack_from('<II', bytes(self.data), 4), (2, 0))
        for command, expected in [(8, errno.EACCES), (5, errno.EACCES), (99, -1)]:
            with self.subTest(command=command):
                self.request(command, b'persist/x', 0, 1)
                self.assertEqual(self.serve()[0], expected)
        self.assertEqual(self.lib.goodix_file_service(self.fd, self.data, 4096, None),
                         -errno.EMSGSIZE)


if __name__ == '__main__':
    unittest.main()
