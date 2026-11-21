"""Check the Windows debug port descriptions against the actual Piano board DT."""
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt
from firmware import pakala_debug


class PakalaDebugTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.board = read_fdt((ROOT / 'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        cls.inputs = ROOT / 'uefi/components/acpi/pakala'
        serial = cls.board['/aliases']['serial0'].rstrip(b'\0').decode()
        cls.uart = cls.board[serial]
        cls.usb = cls.board['/soc/ssusb@a600000']

    def table(self, signature):
        original = (self.inputs / (signature + '.aml')).read_bytes()
        table = pakala_debug.prepare(original, self.board)
        self.assertEqual(struct.unpack_from('<4sI', table), (signature.encode(), len(table)))
        self.assertEqual(sum(table) % 256, 0)
        self.assertEqual(pakala_debug.prepare(table, self.board), table)
        return table

    def test_console_matches_geni_and_the_board_interrupt(self):
        table = self.table('SPCR')
        uart, _ = struct.unpack('>II', self.uart['reg'])
        _, spi, _ = struct.unpack('>III', self.uart['interrupts'])
        self.assertEqual(table[36], 0x13)  # Microsoft's GENI 7.3728 MHz subtype.
        self.assertEqual(struct.unpack_from('<BBBBQ', table, 40), (0, 32, 0, 3, uart))
        self.assertEqual(struct.unpack_from('<BBIBBB', table, 52), (8, 0, 32 + spi, 7, 0, 1))

    def test_dbg2_describes_only_existing_ports_and_fixups(self):
        data = self.table('DBG2')
        offset, count = struct.unpack_from('<II', data, 36)
        self.assertEqual((data[8], count), (0, 3))
        uart, uart_bytes = struct.unpack('>II', self.uart['reg'])
        usb, usb_bytes = struct.unpack_from('>II', self.usb['reg'])
        modes = []
        for i in range(count):
            rev, length, nreg = struct.unpack_from('<BHB', data, offset)
            self.assertEqual(rev, 0)
            row = data[offset:offset+length]
            offset += length
            name_len, name_at, oem_len, oem_at, kind, subtype, _, addr_at, size_at = struct.unpack_from('<9H', row, 4)
            self.assertLessEqual(name_at + name_len, length)
            name = row[name_at:name_at+name_len]
            addresses = []
            for n in range(nreg):
                space, bits, bit_offset, access, base = struct.unpack_from('<BBBBQ', row, addr_at + n*12)
                self.assertEqual((space, bits, bit_offset, access), (0, 32, 0, 3))
                addresses.append((base, struct.unpack_from('<I', row, size_at + n*4)[0]))
            if i == 0:
                self.assertEqual((kind, subtype, name, oem_len, oem_at), (0x8000, 0x13, b'.\0', 0, 0))
                self.assertEqual(addresses, [(uart, uart_bytes)])
                continue
            self.assertEqual((kind, subtype, name), (0x8003, 0x5143, b'\\_SB.URS0\0'))
            self.assertEqual(addresses, [(usb, usb_bytes), (usb, 0x1000)])
            self.assertEqual(oem_at + oem_len, length)
            oem = row[oem_at:]
            port, unused, signature, writes = struct.unpack_from('<HH4sI', oem)
            self.assertEqual((port, unused, signature, writes), (5, 0, b'2XIF', 2))
            steps = list(struct.iter_unpack('<BBHIII', oem[12:12 + writes*16]))
            self.assertTrue(all(bar == 0 and rsvd == 0 and reg + 4 <= usb_bytes
                                for bar, _, rsvd, reg, _, _ in steps))
            self.assertEqual(steps[1][3:], (0xf8810, 0xffffffff, (1 << 28) | (1 << 20)))
            self.assertEqual(oem[12 + writes*16:-4], bytes(oem_len - 12 - writes*16 - 4))
            self.assertEqual(oem[-4:], b'CBSU')
            modes.append((steps[0][1:], steps[1][1]))
        self.assertEqual(offset, len(data))
        self.assertEqual(modes, [((2, 0, 0xc700, 0xfffffff8, 0), 2),
                                 ((2, 0, 0xc704, 0xffffe1ff, 0), 1)])

    def test_other_platform_resource_tables_are_untouched(self):
        for name in ('CSRT', 'FACS', 'FACP'):
            table = (self.inputs / (name + '.aml')).read_bytes()
            self.assertEqual(pakala_debug.prepare(table, self.board), table)


if __name__ == '__main__':
    unittest.main()
