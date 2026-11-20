"""Check the compiled ARM tables against Piano's independent firmware descriptions."""
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
from prepare_product import acpi_smmu_resources


def cell(value):
    return int.from_bytes(value, 'big')


class AcpiTablesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        include = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
        arch = {'aarch64': 'AArch64', 'x86_64': 'X64'}.get(platform.machine())
        if not (arch and include.is_dir() and shutil.which('cc') and shutil.which('objcopy')):
            raise unittest.SkipTest('Native compiler, objcopy and the pinned MdePkg headers are required')
        source = ROOT / 'uefi/components/acpi'
        dtbs = ROOT / 'vendor/piano/dtb'
        cls.board = read_fdt((dtbs / 'piano.dtb').read_bytes())['tree']
        cls.xbl = read_fdt((dtbs / 'xbl-config.dtb').read_bytes())['tree']
        cls.tables = {}
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            (work / 'SmmuResources.h').write_text(acpi_smmu_resources(cls.board, cls.xbl))
            for name in ('Madt', 'Iort'):
                subprocess.run(['cc', '-c', '-x', 'c', '-fshort-wchar', '-include', 'Uefi.h',
                                '-I', include, '-I', include / arch, '-I', work, source / (name+'.aslc'),
                                '-o', work / 'table.o'], check=True)
                subprocess.run(['objcopy', '-j', '.data', '-O', 'binary', work / 'table.o',
                                work / 'table.bin'], check=True)
                cls.tables[name] = (work / 'table.bin').read_bytes()

    def test_madt_describes_every_cpu_and_redistributor(self):
        madt = self.tables['Madt']
        self.assertEqual(struct.unpack_from('<4sI', madt), (b'APIC', len(madt)))
        gic = self.board['/soc/interrupt-controller@16000000']
        dist_base, _, redistrib_base, _ = struct.unpack('>4I', gic['reg'])
        stride = cell(gic['redistributor-stride'])
        cpus = sorted(cell(p['reg']) for path, p in self.board.items()
                      if path.startswith('/cpus/cpu@') and p.get('device_type') == b'cpu\0')
        found = []
        pos = 44
        while pos < len(madt):
            kind, size = struct.unpack_from('<BB', madt, pos)
            self.assertGreaterEqual(size, 2)
            if kind == 11:
                uid, = struct.unpack_from('<I', madt, pos+8)
                gicr, mpidr = struct.unpack_from('<QQ', madt, pos+60)
                self.assertEqual(gicr, redistrib_base + uid*stride)
                found.append((uid, mpidr))
            elif kind == 12:
                self.assertEqual(struct.unpack_from('<Q', madt, pos+8), (dist_base,))
            pos += size
        self.assertEqual(pos, len(madt))
        self.assertEqual(sorted(found), list(enumerate(cpus)))

    def test_iort_dma_paths_and_context_irqs_match_xbl(self):
        iort = self.tables['Iort']
        self.assertEqual(struct.unpack_from('<4sI', iort), (b'IORT', len(iort)))
        count, pos = struct.unpack_from('<II', iort, 36)
        nodes = {}
        for _ in range(count):
            kind, length = struct.unpack_from('<BH', iort, pos)
            self.assertGreaterEqual(length, 16)
            nodes[pos] = (kind, iort[pos:pos+length])
            pos += length
        self.assertEqual(pos, len(iort))
        named = {}
        for kind, node in nodes.values():
            if kind != 1:
                continue
            maps, offset = struct.unpack_from('<II', node, 8)
            self.assertEqual(maps, 1)
            path = node[29:offset].split(b'\0', 1)[0].decode()
            _, _, sid, smmu_offset, flags = struct.unpack_from('<5I', node, offset)
            self.assertEqual(flags, 1)  # Single mapping for a non-PCI requester.
            named[path] = sid
            smmu_type, smmu = nodes[smmu_offset]
            self.assertEqual(smmu_type, 3)
        self.assertEqual(set(named), {'\\_SB_.UFS0', '\\_SB_.URS0'})
        for path, xbl_name in (('\\_SB_.UFS0', 'NAMEDNODE_UFS_95MEM'), ('\\_SB_.URS0', 'NAMEDNODE_USB0')):
            route = self.xbl['/soc/iort/'+xbl_name+'/SIDMappings/SIDMappings_0']
            self.assertEqual(named[path], cell(route['OutputBase']))
        xbl = self.xbl['/soc/iort/APPS_MMU500_SMMU_APP']
        global_off, count, context_off = struct.unpack_from('<III', smmu, 40)
        self.assertEqual(struct.unpack_from('<I', smmu, global_off), (cell(xbl['NSGIRPT_GSIV']),))
        interrupts = [struct.unpack_from('<I', smmu, context_off + 8*i)[0] for i in range(count)]
        self.assertEqual(interrupts, [v for (v,) in struct.iter_unpack('>I', xbl['ContextInterrupts'])])
        msi_offset, = struct.unpack_from('<I', smmu, 12)
        in_id, _, out_id, its_offset, _ = struct.unpack_from('<5I', smmu, msi_offset)
        self.assertEqual(in_id, named['\\_SB_.UFS0'])
        self.assertEqual(out_id, cell(self.board['/soc/ufshc@1d84000']['msi-parent'][4:]))
        self.assertEqual(nodes[its_offset][0], 0)
