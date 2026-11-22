"""Check the compiled ACPI tables against the independently stored board DT."""
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


def cells(value):
    return struct.unpack('>%dI' % (len(value) // 4), value)


def gsi(irq):
    kind, number, _ = irq
    if kind != 1:
        raise AssertionError('Expected a GIC PPI')
    return 16 + number


class AcpiTablesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        include = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
        arch = {'aarch64': 'AArch64', 'x86_64': 'X64'}.get(platform.machine())
        if not (arch and include.is_dir() and shutil.which('cc') and shutil.which('objcopy')):
            raise unittest.SkipTest('Native compiler, objcopy and the pinned MdePkg headers are required')
        source = ROOT / 'uefi/components/acpi'
        dtb = ROOT / 'vendor/piano/dtb/piano.dtb'
        cls.board = read_fdt(dtb.read_bytes())['tree']
        cls.tables = {}
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            for name, signature in (('Fadt', b'FACP'), ('Gtdt', b'GTDT'), ('Madt', b'APIC')):
                subprocess.run(['cc', '-c', '-x', 'c', '-fshort-wchar', '-Wall', '-Werror',
                                '-include', 'Uefi.h', '-I', include, '-I', include / arch,
                                source / (name + '.aslc'), '-o', work / 'table.o'], check=True)
                subprocess.run(['objcopy', '-j', '.data', '-O', 'binary', work / 'table.o',
                                work / 'table.bin'], check=True)
                data = (work / 'table.bin').read_bytes()
                if struct.unpack_from('<4sI', data) != (signature, len(data)):
                    raise AssertionError('Unexpected ACPI table signature or size')
                cls.tables[name] = data

    def node(self, compatible):
        node, = (props for props in self.board.values()
                 if compatible in props.get('compatible', b'').split(b'\0'))
        return node

    def test_fadt_advertises_psci_and_hardware_reduced_acpi(self):
        fadt = self.tables['Fadt']
        self.assertEqual(self.node(b'arm,psci-1.0')['method'], b'smc\0')
        self.assertEqual(struct.unpack_from('<I', fadt, 112), (1 << 20,))
        self.assertEqual(struct.unpack_from('<H', fadt, 129), (1,))

    def test_gtdt_uses_the_board_timer_ppis(self):
        gtdt = self.tables['Gtdt']
        timer = self.node(b'arm,armv8-timer')
        rows = tuple(struct.iter_unpack('>III', timer['interrupts']))
        self.assertEqual(len(rows), 4)
        expected = []
        for irq in rows:
            sense = irq[2] & 15
            self.assertIn(sense, (1, 2, 4, 8))
            flags = (sense in (1, 2)) | ((sense in (2, 8)) << 1) | (('always-on' in timer) << 2)
            expected += [gsi(irq), flags]
        self.assertEqual(struct.unpack_from('<8I', gtdt, 48), tuple(expected))
        self.assertEqual(struct.unpack_from('<Q', gtdt, 36), (2**64 - 1,))
        self.assertEqual(struct.unpack_from('<Q', gtdt, 80), (2**64 - 1,))

    def test_madt_describes_all_cpu_affinities_and_interrupt_interfaces(self):
        madt = self.tables['Madt']
        gic = self.node(b'arm,gic-v3')
        distributor, _, redistributors, length = cells(gic['reg'])
        stride = int.from_bytes(gic['redistributor-stride'], 'big')
        pmu = gsi(cells(self.node(b'arm,armv8-pmuv3')['interrupts']))
        maintenance = gsi(cells(gic['interrupts']))
        cpus = sorted(int.from_bytes(p['reg'], 'big') for path, p in self.board.items()
                      if path.startswith('/cpus/cpu@') and p.get('device_type') == b'cpu\0')
        its_address = cells(self.node(b'arm,gic-v3-its')['reg'])[0]
        actual = []
        distributor_count = its_count = 0
        pos = 44
        while pos < len(madt):
            kind, size = struct.unpack_from('<BB', madt, pos)
            self.assertGreaterEqual(size, 2)
            if kind == 11:
                uid, flags, _, performance_irq = struct.unpack_from('<IIII', madt, pos + 8)
                gicr, mpidr = struct.unpack_from('<QQ', madt, pos + 60)
                self.assertEqual(flags, 1)
                self.assertEqual(performance_irq, pmu)
                self.assertEqual(struct.unpack_from('<I', madt, pos + 56), (maintenance,))
                self.assertEqual(gicr, redistributors + uid * stride)
                self.assertLess(gicr, redistributors + length)
                actual.append((uid, mpidr))
            elif kind == 12:
                distributor_count += 1
                self.assertEqual(struct.unpack_from('<Q', madt, pos + 8), (distributor,))
                self.assertEqual(madt[pos + 20], 3)
            elif kind == 15:
                its_count += 1
                self.assertEqual(struct.unpack_from('<Q', madt, pos + 8), (its_address,))
            else:
                self.fail(f'Unexpected MADT entry type {kind}')
            pos += size
        self.assertEqual(pos, len(madt))
        self.assertEqual(distributor_count, 1)
        self.assertEqual(its_count, 1)
        self.assertEqual(sorted(actual), list(enumerate(cpus)))
