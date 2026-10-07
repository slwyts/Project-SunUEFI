"""PC proposal geometry and preservation checks against the real GPT backup."""
from pathlib import Path
import struct
import sys
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import prepare_product_storage as product


class ProductStorageProposalTests(unittest.TestCase):
    def setUp(self):
        self.blobs = product.verify_capture()
        self.identity = uuid.UUID('13f0e622-2c1e-469c-9907-9d98dd3a2a53')

    def test_existing_partitions_and_header_fields_preserved(self):
        result = product.proposed_gpt(self.blobs, self.identity)
        before, after = self.blobs['primary-entries.bin'], result['primary-entries.bin']
        self.assertEqual(after, result['backup-entries.bin'])
        for index in range(96):
            if index != 95:
                self.assertEqual(before[index*128:(index+1)*128], after[index*128:(index+1)*128])
        entry = after[95*128:96*128]
        self.assertEqual(entry[:16], product.TYPE_GUID.bytes_le)
        self.assertEqual(entry[16:32], self.identity.bytes_le)
        self.assertEqual(struct.unpack_from('<QQQ', entry, 32), (375040, 378623, 2))
        self.assertEqual(entry[56:].decode('utf-16le').rstrip('\0'), 'PianoUEFI Storage')
        for name, here in (('primary-header.bin', 1), ('backup-header.bin', product.LAST_DISK_LBA)):
            parsed = product.parse_gpt_header(result[name], here, product.LAST_DISK_LBA)
            self.assertEqual(parsed['array_crc'], zlib.crc32(after))
            self.assertEqual(parsed['entries'], 96)
            for index, pair in enumerate(zip(self.blobs[name], result[name])):
                if pair[0] != pair[1]:
                    self.assertTrue(16 <= index < 20 or 88 <= index < 92)

    def test_nonempty_reserved_entry_is_refused(self):
        altered = dict(self.blobs)
        entries = bytearray(altered['primary-entries.bin'])
        entries[95*128] = 1
        altered['primary-entries.bin'] = bytes(entries)
        with self.assertRaises(ValueError):
            product.proposed_gpt(altered, self.identity)

    def test_immutable_volume_header_matches_shared_wire_layout(self):
        disk = self.blobs['primary-header.bin'][56:72]
        block = bytearray(product.volume_header(self.identity, disk))
        self.assertEqual(len(block), 4096)
        self.assertEqual(block[:16], b'PIANO-VOLUME-v1\0')
        self.assertEqual(block[32:48], self.identity.bytes_le)
        self.assertEqual(block[48:64], disk)
        self.assertEqual(block[64:80], product.TYPE_GUID.bytes_le)
        self.assertEqual(struct.unpack_from('<IIQIIIIIII', block, 80),
                         (4, 4096, 375040, 3584, 2, 2046, 2048, 2816, 768, 95))
        self.assertFalse(any(block[124:]))
        checksum = struct.unpack_from('<I', block, 24)[0]
        struct.pack_into('<I', block, 24, 0)
        self.assertEqual(zlib.crc32(block), checksum)

    def test_journal_commit_is_last_and_binds_payload_and_volume(self):
        payload = bytes((index*7+29) & 255 for index in range(product.SNAPSHOT_BYTES))
        slot = product.journal_slot(payload, self.identity)
        self.assertEqual(len(slot), 3*1024*1024)
        self.assertEqual(slot[4096:4096+len(payload)], payload)
        header = bytearray(slot[:4096])
        footer = bytearray(slot[767*4096:])
        self.assertEqual(header[64:80], self.identity.bytes_le)
        self.assertEqual(footer[64:80], self.identity.bytes_le)
        self.assertEqual(struct.unpack_from('<Q', header, 40)[0], 1)
        self.assertEqual(struct.unpack_from('<I', header, 52)[0], zlib.crc32(payload))
        self.assertEqual(struct.unpack_from('<I', footer, 56)[0], struct.unpack_from('<I', header, 24)[0])
        for block in (header, footer):
            checksum = struct.unpack_from('<I', block, 24)[0]
            struct.pack_into('<I', block, 24, 0)
            self.assertEqual(checksum, zlib.crc32(block))
        self.assertFalse(any(slot[4096+len(payload):767*4096]))
        with self.assertRaises(ValueError):
            product.journal_slot(payload[:-1], self.identity)


if __name__ == '__main__':
    unittest.main()
