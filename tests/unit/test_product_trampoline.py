import importlib.util
from pathlib import Path
import struct
import unittest
import hashlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('trampoline', ROOT / 'tools/package_product_trampoline.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def original():
    result = bytearray(8192)
    struct.pack_into('<IIQQQ', result, 0, 0xfa405a4d, 0x14000010, 0, 16384, 10)
    result[56:60] = b'ARMd'
    result[68:] = bytes((index * 29) & 255 for index in range(len(result)-68))
    return bytes(result)


def components():
    shim = bytearray(128);shim[56:60] = b'ARMd'
    fd = bytes(0x300000)
    code = b'MZ' + bytes(4094)
    app = struct.pack('<16sIIQ32s', b'SUNUEFI-APPv1\0', 1, 64, len(code), hashlib.sha256(code).digest()) + code
    return bytes(shim), fd, app


class TrampolinePackageTests(unittest.TestCase):
    def test_original_kernel_and_bss_are_preserved_and_restorable(self):
        stock = original();shim, fd, app = components()
        selector = bytes(512);symbols = {'__image_end': 512+8192, 'bootselect_metadata': 256}
        blob, layout = module.combine(stock, selector, symbols, shim, fd, app)
        self.assertEqual(blob[64:len(stock)], stock[64:])
        self.assertEqual(blob[len(stock):16384], bytes(16384-len(stock)))
        self.assertTrue(layout['original_kernel_reconstructable_byte_equal'])
        self.assertGreaterEqual(layout['bootshim_offset'], layout['selector_offset'] + symbols['__image_end'])
        self.assertEqual(blob[layout['fd_offset']:layout['fd_offset']+len(fd)], fd)
        self.assertEqual(blob[layout['app_offset']:layout['app_offset']+len(app)], app)
        branch = struct.unpack_from('<I', blob, 4)[0]
        self.assertEqual(4 + (branch & 0x3ffffff)*4, layout['selector_offset'])
        self.assertEqual(struct.unpack_from('<Q', blob, 16)[0], len(blob))

    def test_bad_app_and_oversized_span_are_rejected(self):
        shim, fd, app = components();symbols = {'__image_end': 8704, 'bootselect_metadata': 256}
        broken = bytearray(app);broken[-1] ^= 1
        with self.assertRaises(ValueError):
            module.combine(original(), bytes(512), symbols, shim, fd, broken)
        large = bytearray(original());struct.pack_into('<Q', large, 16, module.BOOT_BYTES)
        with self.assertRaises(ValueError):
            module.combine(large, bytes(512), symbols, shim, fd, app)

    def test_boot4_extraction_rejects_wrapped_or_wrong_format(self):
        kernel = original();header = bytearray(4096);header[:8] = b'ANDROID!'
        struct.pack_into('<I', header, 8, len(kernel));struct.pack_into('<I', header, 20, 1584)
        struct.pack_into('<I', header, 40, 4)
        extracted, record = module.stock_kernel(header + kernel)
        self.assertEqual(extracted, kernel)
        self.assertEqual(record['kernel_sha256'], hashlib.sha256(kernel).hexdigest())
        embedded = bytearray(kernel);embedded[4096:4112] = b'SUNUEFI-SPLITv1\0'
        with self.assertRaises(ValueError):
            module.stock_kernel(header + embedded)
        struct.pack_into('<I', header, 1580, 4096)
        with self.assertRaises(ValueError):
            module.stock_kernel(header + kernel)


if __name__ == '__main__':
    unittest.main()
