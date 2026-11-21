"""Submit the Windows transfer descriptors to QEMU's UFSHCI and check the raw LUN."""
import ctypes as ct
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'windows/drivers/ufs'


@unittest.skipUnless(shutil.which('qemu-system-aarch64'), 'requires an ARM QEMU with the UFS model')
class UfsControllerTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix='sunuefi-ufs-')
        self.addCleanup(work.cleanup)
        directory = Path(work.name)
        self.disk = directory / 'lun.img'
        with self.disk.open('xb') as image:
            image.truncate(64 * 1024 * 1024)
        library = directory / 'driver.so'
        subprocess.run(['cc', '-std=c17', '-Werror', '-Wall', '-shared', '-fPIC', '-I', DRIVER,
                        DRIVER / 'transfer.c', ROOT / 'tests/native/UfsControllerTest.c', '-o', library],
                       check=True)
        self.driver = ct.CDLL(str(library))
        self.driver.build_request.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint64, ct.c_void_p,
                                              ct.c_size_t, ct.c_int, ct.POINTER(ct.c_uint64),
                                              ct.POINTER(ct.c_uint32), ct.c_uint]
        self.driver.read_completion.argtypes = [ct.c_void_p, ct.c_void_p, ct.POINTER(ct.c_uint32),
                                                ct.POINTER(ct.c_uint8), ct.POINTER(ct.c_uint8)]
        # No guest OS or firmware: QTest drives the controller, with the CPU stopped.
        self.qemu = subprocess.Popen(
            ['qemu-system-aarch64', '-machine', 'virt,highmem=off', '-cpu', 'cortex-a53',
             '-accel', 'tcg', '-S', '-smp', '1', '-m', '64', '-nodefaults', '-display', 'none',
             '-monitor', 'none', '-serial', 'none', '-nic', 'none', '-qtest', 'stdio',
             '-qtest-log', '/dev/null', '-device', 'ufs,addr=1.0,id=controller,serial=SUNUEFI-TEST',
             '-drive', f'if=none,id=lun,format=raw,file={self.disk}',
             '-device', 'ufs-lu,bus=controller,drive=lun,lun=0'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)

    def stop(self):
        self.qemu.terminate()
        try:
            self.qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.qemu.kill()
            self.qemu.wait()
        self.qemu.stdin.close()
        self.qemu.stdout.close()

    def command(self, text):
        self.qemu.stdin.write(text + '\n')
        self.qemu.stdin.flush()
        line = self.qemu.stdout.readline().strip()
        self.assertTrue(line == 'OK' or line.startswith('OK '), (text[:72], line[:72]))
        return line[2:].strip()

    def read32(self, address):
        return int(self.command(f'readl {address:#x}'), 16)

    def write32(self, address, value):
        self.command(f'writel {address:#x} {value:#x}')

    def read(self, address, size):
        return bytes.fromhex(self.command(f'read {address:#x} {size:#x}')[2:])

    def write(self, address, data):
        self.command(f'write {address:#x} {len(data):#x} 0x{data.hex()}')

    # Addresses belong to QEMU virt's memory map, not to Piano's ACPI or hardware.
    hci = 0x10000000
    list_addr, ucd_addr = 0x41200000, 0x41202000
    pages = (0x41400000, 0x41500000)

    def submit(self, cdb, size=0, payload=None):
        utrd = ct.create_string_buffer(32)
        ucd = ct.create_string_buffer(256 + 2 * 16)
        sizes = ((size // 2) & ~3, size - ((size // 2) & ~3)) if size else ()
        direction = 0 if not size else 2 if payload is None else 1
        self.assertTrue(self.driver.build_request(utrd, ucd, self.ucd_addr, cdb, len(cdb), direction,
                                                  (ct.c_uint64 * 2)(*self.pages), (ct.c_uint32 * 2)(*sizes),
                                                  len(sizes)))
        offset = 0
        for address, length in zip(self.pages, sizes):
            self.write(address, b'\xcc' * length if payload is None else payload[offset:offset + length])
            offset += length
        self.write(self.ucd_addr, ucd.raw)
        self.write(self.list_addr, utrd.raw)
        self.write32(self.hci + 0x58, 1)
        deadline = time.monotonic() + 5
        while self.read32(self.hci + 0x58) & 1:
            self.assertLess(time.monotonic(), deadline, 'UFS controller did not complete the request')
            time.sleep(.002)
        self.assertTrue(self.read32(self.hci + 0x20) & 1, 'missing completion interrupt')
        self.write32(self.hci + 0x20, 1)
        utrd.raw = self.read(self.list_addr, ct.sizeof(utrd))
        ucd.raw = self.read(self.ucd_addr, ct.sizeof(ucd))
        transferred, scsi, ocs = ct.c_uint32(), ct.c_uint8(), ct.c_uint8()
        result = self.driver.read_completion(utrd, ucd, ct.byref(transferred), ct.byref(scsi), ct.byref(ocs))
        self.assertEqual(ocs.value, 0, cdb.hex())
        data = b''.join(self.read(address, length) for address, length in zip(self.pages, sizes))
        return result, scsi.value, data[:transferred.value]

    def test_write_flush_and_split_dma_read(self):
        # Enable QEMU's PCI function. The driver's command descriptors are bus-independent.
        cfg = 0x3f000000 + (1 << 15)
        self.assertEqual(self.read32(cfg + 8) >> 8, 0x010901)
        self.write32(cfg + 0x10, self.hci)
        self.write32(cfg + 0x14, 0)
        self.command(f'writew {cfg + 4:#x} 0x6')
        self.write32(self.hci + 0x34, 1)
        self.assertTrue(self.read32(self.hci + 0x30) & 8)  # UIC command ready
        self.write32(self.hci + 0x90, 0x16)  # DME_LINKSTARTUP
        self.assertEqual(self.read32(self.hci + 0x30) & 3, 3)
        self.assertEqual(self.read32(self.hci + 0x98) & 0xff, 0)
        self.write32(self.hci + 0x20, self.read32(self.hci + 0x20))
        self.write32(self.hci + 0x50, self.list_addr)
        self.write32(self.hci + 0x54, 0)
        self.write32(self.hci + 0x60, 1)
        self.write32(self.hci + 0x24, 1)
        # TEST UNIT READY may first report power-on unit attention.
        if self.submit(bytes(6))[:2] != (0, 0):
            self.assertEqual(self.submit(bytes(6))[:2], (0, 0))
        result, status, capacity = self.submit(bytes([0x25]) + bytes(9), 8)
        self.assertEqual((result, status), (0, 0))
        last, sector = struct.unpack('>II', capacity)
        self.assertEqual((last + 1) * sector, self.disk.stat().st_size)
        block, count = 13, 2
        payload = hashlib.shake_256(b'Windows UFS request readback').digest(count * sector)
        def rw(op):
            return struct.pack('>BBIBHB', op, 0, block, 0, count, 0)
        self.assertEqual(self.submit(rw(0x2a), len(payload), payload)[:2], (0, 0))
        self.assertEqual(self.submit(bytes([0x35]) + bytes(9))[:2], (0, 0))
        self.assertEqual(self.submit(rw(0x28), len(payload)), (0, 0, payload))
        with self.disk.open('rb') as image:
            image.seek((block - 1) * sector)
            self.assertEqual(image.read((count + 2) * sector), bytes(sector) + payload + bytes(sector))


if __name__ == '__main__':
    unittest.main()
