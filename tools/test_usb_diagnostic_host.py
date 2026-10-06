#!/usr/bin/env python3
"""Offline reader validation; these tests never initialize libusb or a device."""
import ctypes
import struct
import unittest
import zlib
import read_usb_diagnostic as reader


class FirmwareModel:
    def __init__(self, log=None, fault=None):
        self.log = log if log is not None else bytes(range(256)) * 4 + bytes(range(40))
        self.fault = fault
        self.calls, self.replies, self.status_reads = [], 0, 0
        self.timeout_sent = False

    def control(self, kind, request, value, page, length):
        self.calls.append((kind, request, value, page, length))
        assert kind == 0xC0 and request in (0x5A, 0x5B) and 0 < length <= 536
        if request == 0x5A:
            return b"SUNUEFI1\x01\x01\x01\x40"
        if self.fault == "timeout" and value == 2 and not self.timeout_sent:
            self.timeout_sent = True
            raise reader.UsbError(-7, "mock page")
        self.replies += 1
        if value == 0:
            self.status_reads += 1
            address = 2 if self.fault == "reset" and self.status_reads > 1 else 1
            count = 1 if self.fault == "stale_status" else self.replies
            return struct.pack("<8s4B9I", b"SUNDBG01", address, 1, 1, 0,
                               0x00600804 | address << 3, 0x253DC, 0x102005,
                               0x10100000, 0x01000000, 3, len(self.calls), count, 3)
        if value == 1:
            total = reader.MAX_LOG_BYTES + 1 if self.fault == "oversize" else len(self.log)
            checksum = zlib.crc32(self.log) ^ (1 if self.fault == "total_crc" else 0)
            return struct.pack("<8sIIIHH", b"SUNLOG01", 7, total, checksum, 512, 0)
        payload = self.log[page*512:(page+1)*512]
        token = 8 if self.fault == "generation" else 7
        crc = zlib.crc32(payload) ^ (1 if self.fault == "page_crc" else 0)
        data = struct.pack("<8sIIHHI", b"SUNPAGE1", token, page*512, len(payload), 24, crc) + payload
        return data[:-1] if self.fault == "short_page" else data


class ReaderTests(unittest.TestCase):
    def test_binary_roundtrip_exact_final_page_length(self):
        model = FirmwareModel()
        result, log = reader.read_roundtrip(model.control)
        self.assertEqual(log, model.log)
        self.assertTrue(result["debug_verified"] and result["log_verified"])
        self.assertEqual(result["pages"], 3)
        pages = [call for call in model.calls if call[1:3] == (0x5B, 2)]
        self.assertEqual([call[4] for call in pages], [536, 536, 64])
        self.assertGreater(result["status_after"]["accepted_replies"], result["status_before"]["accepted_replies"])

    def test_full_256k_snapshot_uses_all_512_vendor_pages(self):
        model = FirmwareModel(log=bytes(range(256)) * 1024)
        result, log = reader.read_roundtrip(model.control)
        self.assertEqual(log, model.log)
        self.assertEqual(result['pages'], 512)
        pages = [call for call in model.calls if call[1:3] == (0x5B, 2)]
        self.assertEqual(pages[-1][3:], (511, 536))
        self.assertEqual(reader.MAX_LOG_BYTES, 262144)

    def test_empty_snapshot(self):
        result, log = reader.read_roundtrip(FirmwareModel(log=b"").control)
        self.assertEqual(log, b"")
        self.assertEqual(result["pages"], 0)
        self.assertEqual(result["log_crc32"], "00000000")

    def test_transient_timeout_retries_same_page(self):
        model = FirmwareModel(fault="timeout")
        result, log = reader.read_roundtrip(model.control)
        self.assertEqual(log, model.log)
        pages = [call for call in model.calls if call[1:3] == (0x5B, 2)]
        self.assertEqual(pages[0], pages[1])
        self.assertEqual(result["request_attempts"], 8)

    def test_reject_corrupt_or_stale_response(self):
        for fault in ("generation", "page_crc", "total_crc", "short_page", "reset", "stale_status", "oversize"):
            with self.subTest(fault=fault), self.assertRaises(reader.ProtocolError):
                reader.read_roundtrip(FirmwareModel(fault=fault).control)

    def test_fixed_direction_and_bounds(self):
        usb = reader.Libusb.__new__(reader.Libusb)
        for args in ((0x40, 0x5B, 0, 0, 48), (0xC0, 0xFF, 0, 0, 48), (0xC0, 0x5B, 0, 0, 65535)):
            with self.assertRaises(reader.ProtocolError):
                usb.control(*args)
        self.assertEqual(ctypes.sizeof(reader.DeviceDescriptor), 18)


if __name__ == "__main__":
    unittest.main()
