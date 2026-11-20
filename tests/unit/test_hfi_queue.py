"""Check the Windows driver's Gen8 HFI code against the shared-memory wire format."""
import ctypes as ct
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest


DRIVER = Path(__file__).resolve().parents[2] / 'windows/drivers/adreno'
U32 = ct.c_uint32
SIZE = ct.c_size_t
P32 = ct.POINTER(U32)
OK, EMPTY, FULL, INVALID, SHORT, BAD, UNMATCHED, FAULT, NOTIFY = range(9)
BYTES = 13136
RING = 212
IOVA = 0x60008000


class Hfi(ct.Structure):
    _fields_ = [('memory', P32), ('sequence', U32)]


class HfiQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = TemporaryDirectory(prefix='piano-hfi-')
        cls.addClassCleanup(temp.cleanup)
        library = Path(temp.name) / 'hfi.so'
        subprocess.run(['cc', '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        DRIVER / 'hfi_queue.c', '-o', library], check=True)
        cls.lib = ct.CDLL(str(library))
        phfi = ct.POINTER(Hfi)
        cls.lib.adreno_hfi_init.argtypes = [phfi, ct.c_void_p, SIZE, ct.c_uint64]
        cls.lib.adreno_hfi_send.argtypes = [phfi, ct.c_uint8, P32, SIZE, P32]
        cls.lib.adreno_hfi_receive.argtypes = [phfi, ct.c_int, P32, SIZE, ct.POINTER(SIZE)]
        cls.lib.adreno_hfi_reply.argtypes = [U32, P32, SIZE, P32, P32, SIZE, ct.POINTER(SIZE)]

    def setUp(self):
        self.page = (U32 * (BYTES // 4))()
        ct.memset(self.page, 0x5a, BYTES)
        self.hfi = Hfi()
        self.assertEqual(self.lib.adreno_hfi_init(ct.byref(self.hfi), self.page, BYTES, IOVA), OK)

    def send(self, msg, *data):
        token = U32(0xbeef)
        payload = (U32 * len(data))(*data) if data else None
        status = self.lib.adreno_hfi_send(ct.byref(self.hfi), msg, payload, len(data), ct.byref(token))
        return status, token.value

    def receive(self, inbox, capacity):
        data = (U32 * capacity)() if capacity else None
        words = SIZE()
        status = self.lib.adreno_hfi_receive(ct.byref(self.hfi), inbox, data, capacity, ct.byref(words))
        return status, words.value, list(data[:words.value]) if data is not None else []

    def reply(self, token, packet, capacity=2):
        data = (U32 * capacity)(*[0xffff] * capacity) if capacity else None
        result, words = U32(), SIZE()
        status = self.lib.adreno_hfi_reply(token, (U32 * len(packet))(*packet), len(packet),
                                          ct.byref(result), data, capacity, ct.byref(words))
        return status, result.value, words.value, list(data) if data is not None else []

    def test_gen8_table_matches_extended_descriptors_and_ring_addresses(self):
        self.assertEqual(self.page[:6], [0, 210, 6, 12, 3, 3])
        for index, address, typ in [(0, 0x60008350, 0x0a00), (1, 0x60009350, 0x0a01),
                                    (2, 0x6000a350, 0x2802)]:
            first = 6 + 12 * index
            self.assertEqual(self.page[first:first + 12], [1, address, typ, 1024] + [0] * 8)
        self.assertEqual(set(self.page[42:RING]), {0})  # Inactive dispatch headers and alignment.
        self.assertEqual(self.page[RING], 0x5a5a5a5a)
        for iova, size in [(0x100000010, BYTES), (0xfffffff0, BYTES), (IOVA + 4, BYTES),
                           (IOVA, BYTES - 1)]:
            with self.subTest(iova=iova, size=size):
                before = bytes(self.page)
                self.assertEqual(self.lib.adreno_hfi_init(ct.byref(self.hfi), self.page, size, iova),
                                 INVALID)
                self.assertEqual(bytes(self.page), before)
        # Alignment is part of the protocol; a specific 4 KiB slot is not.
        self.assertEqual(self.lib.adreno_hfi_init(ct.byref(self.hfi), self.page, BYTES, 0x10), OK)
        self.assertEqual(self.page[7], 0x360)

    def test_command_bytes_across_ring_end_and_sequence_wrap(self):
        self.assertEqual(self.send(14, 0), (OK, 0x0010020e))  # CORE_FW_START, handle = 0.
        self.assertEqual(self.page[RING:RING + 4], [0x0010020e, 0, 0xfafafafa, 0xfafafafa])
        self.page[16], self.page[17] = 100, 1020
        self.hfi.sequence = 0xffe
        self.assertEqual(self.send(3, 0x11, 0x22, 0x33, 0x44), (OK, 0xfff00503))
        self.assertEqual(self.page[RING + 1020:RING + 1024], [0xfff00503, 0x11, 0x22, 0x33])
        self.assertEqual(self.page[RING:RING + 4], [0x44] + [0xfafafafa] * 3)
        self.assertEqual(self.page[17], 4)
        self.assertEqual(self.send(10), (OK, 0x0000010a))
        self.assertEqual(self.page[17], 8)
        self.assertEqual(self.page[28:30], [0, 0])  # RX and debug queues remain independent.
        self.assertEqual(self.page[40:42], [0, 0])

    def test_full_invalid_and_maximum_commands(self):
        self.page[16], self.page[17] = 8, 0
        before = bytes(self.page)
        self.assertEqual(self.send(3, *range(4)), (FULL, 0xbeef))  # No room for the aligned packet.
        self.assertEqual(self.send(3, *range(255)), (INVALID, 0xbeef))
        self.assertEqual(bytes(self.page), before)
        self.assertEqual(self.hfi.sequence, 0)
        self.page[16] = 0
        self.assertEqual(self.send(3, *range(254)), (OK, 0x0010ff03))
        self.assertEqual(self.page[17], 256)
        self.assertEqual(self.page[RING + 255], 0xfafafafa)
        self.page[16] = 1024
        self.assertEqual(self.send(14, 0), (BAD, 0xbeef))
        self.assertEqual(self.page[17], 256)

    def test_receive_wrap_measure_and_retry(self):
        self.assertEqual(self.receive(1, 0), (EMPTY, 0, []))
        self.assertEqual(self.receive(2, 0), (EMPTY, 0, []))
        self.assertEqual(self.receive(0, 1)[0], INVALID)  # Command ring is not a firmware inbox.
        rx = RING + 1024
        self.page[28], self.page[29] = 1020, 4
        self.page[rx + 1020:rx + 1024] = [0x0011050e, 0x0010020e, 0, 0x77]
        self.page[rx] = 0x66
        self.assertEqual(self.receive(1, 0), (SHORT, 5, []))
        self.assertEqual(self.page[28], 1020)
        self.assertEqual(self.receive(1, 5), (OK, 5, [0x0011050e, 0x0010020e, 0, 0x77, 0x66]))
        self.assertEqual(self.page[28], 4)
        debug = RING + 2048
        self.page[41], self.page[debug] = 4, 0x00000165
        self.assertEqual(self.receive(2, 1), (OK, 1, [0x00000165]))
        self.assertEqual(self.receive(1, 0), (EMPTY, 0, []))

    def test_truncated_and_corrupt_rx_does_not_advance(self):
        for read, write, header in [(0, 4, 0xff00), (0, 4, 0), (2, 4, 0x100),
                                    (1024, 1024, 0x100), (0, 1024, 0x100)]:
            with self.subTest(read=read, write=write, header=header):
                self.page[28], self.page[29] = read, write
                self.page[RING + 1024] = header
                before = bytes(self.page)
                self.assertEqual(self.receive(1, 255)[:2], (BAD, 0))
                self.assertEqual(bytes(self.page), before)

    def test_matching_ack_returns_raw_result_and_command_specific_payload(self):
        token = 0x0010020e
        for echo in (token, token | (1 << 19)):
            self.assertEqual(self.reply(token, [0x0011050e, echo, 0x80000042, 99, 13]),
                             (OK, 0x80000042, 2, [99, 13]))
        self.assertEqual(self.reply(token, [0x0011050e, token, 0, 99, 13], 1),
                         (SHORT, 0, 2, [0xffff]))
        self.assertEqual(self.reply(token, [0x0011030e, token, 0], 0), (OK, 0, 0, []))
        for message, expected in [([0x0001020e, token], BAD),
                                  ([0x0001030e, token, 0, 0], BAD),
                                  ([0x0001030e, token ^ (1 << 20), 0], UNMATCHED),
                                  ([0x0001030e, token ^ 1, 0], UNMATCHED),
                                  ([0x00000265, 0], NOTIFY), ([0x100 | 100], BAD)]:
            with self.subTest(message=message):
                self.assertEqual(self.reply(token, message), (expected, 0, 0, [0xffff, 0xffff]))
        self.assertEqual(self.reply(token, [0x264, 0x12340005]), (FAULT, 0x12340005, 0,
                                                                 [0xffff, 0xffff]))


if __name__ == '__main__':
    unittest.main()
