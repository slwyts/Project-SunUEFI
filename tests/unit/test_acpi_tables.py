"""Check the compiled ARM tables against Piano's independent firmware descriptions."""
import os
import platform
import re
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
from firmware import pakala_acpi
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
            pakala_acpi.prepare(source / 'pakala', work / 'Tables', cls.board)
            cls.tables.update((p.stem, p.read_bytes()) for p in (work / 'Tables').glob('*.bin'))
            subprocess.run(['cc', '-c', '-x', 'c', '-fshort-wchar', '-include', 'Uefi.h',
                            '-I', include, '-I', include / arch, '-I', work, source / 'Iort.aslc',
                            '-o', work / 'table.o'], check=True)
            subprocess.run(['objcopy', '-j', '.data', '-O', 'binary', work / 'table.o',
                            work / 'table.bin'], check=True)
            cls.tables['Iort'] = (work / 'table.bin').read_bytes()

    def test_madt_describes_every_cpu_and_redistributor(self):
        madt = self.tables['APIC']
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

    def mmio_and_irqs(self, device):
        kit = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', ROOT / 'build/host-tools/usr')) / 'bin'
        tools = [shutil.which(str(kit / name)) or shutil.which(name) for name in ('iasl', 'acpiexec')]
        if not all(tools):
            self.skipTest('ACPICA compiler and interpreter are required')
        iasl, acpiexec = tools
        with tempfile.TemporaryDirectory() as folder:
            prefix = Path(folder) / 'piano'
            subprocess.run([iasl, '-we', '-p', prefix, ROOT / 'uefi/components/acpi/Dsdt.asl'],
                           capture_output=True, check=True)
            output = subprocess.check_output([acpiexec, '-b', rf'evaluate \_SB.{device}._CRS',
                                              prefix.with_suffix('.aml')], stderr=subprocess.STDOUT, text=True)
        # ACPICA prints small buffers on the same line as their heading.
        resource = bytes.fromhex(' '.join(re.findall(r'\b[\da-f]+: ([\da-f ]+)//', output, re.I)))
        windows, irqs = [], []
        offset = 0
        while offset < len(resource) and resource[offset] != 0x79:  # EndTag
            kind, length = struct.unpack_from('<BH', resource, offset)
            if kind == 0x86:  # 32-bit fixed memory
                writable, start, size = struct.unpack_from('<BII', resource, offset + 3)
                self.assertEqual(writable, 1)
                windows.append((start, size))
            elif kind == 0x89:  # Extended interrupt
                flags, count = struct.unpack_from('<BB', resource, offset + 3)
                self.assertEqual(flags, 1)  # Consumer, level-high, exclusive
                irqs.extend(struct.unpack_from(f'<{count}I', resource, offset + 5))
            else:
                self.fail(f'Unrecognised {device} resource {kind:#x}')
            offset += 3 + length
        self.assertEqual(resource[offset:], b'\x79\x00')
        return windows, irqs

    def test_gpio_pnp_resources_match_the_board(self):
        gpio = next(n for n in self.board.values()
                    if b'qcom,sun-tlmm' in n.get('compatible', b'').split(b'\0'))
        windows, irqs = self.mmio_and_irqs('TLMM')
        self.assertEqual(windows, list(struct.iter_unpack('>II', gpio['reg'])))
        kind, spi, flags = struct.unpack('>III', gpio['interrupts'])
        self.assertEqual((kind, flags), (0, 4))  # Level-high SPI
        self.assertEqual(irqs, [32 + spi])

    def test_system_clocks_have_one_resource_shared_by_storage_and_gpu(self):
        handles = {n['phandle']: n for n in self.board.values() if 'phandle' in n}
        gcc = self.board['/soc/clock-controller@100000']
        for path in ('/soc/ufshc@1d84000', '/soc/qcom,gmu@3d37000'):
            self.assertTrue(any(handles[handle] is gcc for handle, _ in
                                struct.iter_unpack('>4sI', self.board[path]['clocks'])))
        base, size = struct.unpack('>II', gcc['reg'])
        self.assertEqual(self.mmio_and_irqs('GCC0'), ([(base, size)], []))
        for device in ('UFS0', 'GPU0', 'GCLK'):
            windows, _ = self.mmio_and_irqs(device)
            self.assertTrue(all(b + n <= base or base + size <= b for b, n in windows))

    def test_gpu_pnp_resources_match_the_board(self):
        gpu = self.board['/soc/qcom,kgsl-3d0@3d00000']
        gmu = self.board['/soc/qcom,gmu@3d37000']
        windows, irqs = self.mmio_and_irqs('GPU0')

        def registers(node):
            return dict(zip(node['reg-names'].rstrip(b'\0').split(b'\0'), struct.iter_unpack('>II', node['reg'])))
        kgsl, gmu_reg = registers(gpu), registers(gmu)
        gpucc = self.board['/soc/clock-controller@3d90000']
        gpucc_base, gpucc_bytes = struct.unpack('>II', gpucc['reg'])
        self.assertEqual(self.mmio_and_irqs('GCLK'), ([(gpucc_base, gpucc_bytes)], []))
        # GPUCC supplies four clocks; the two memory bus clocks have a different owner.
        providers = {n['phandle']: path for path, n in self.board.items() if 'phandle' in n}
        names = gmu['clock-names'].rstrip(b'\0').decode().split('\0')
        clocks = {name: (providers[handle], index) for name, (handle, index) in
                  zip(names, struct.iter_unpack('>4sI', gmu['clocks']), strict=True)}
        gcc_node, gpucc_node = '/soc/clock-controller@100000', '/soc/clock-controller@3d90000'
        self.assertEqual(clocks, {'gmu_clk': (gpucc_node, 4), 'cxo_clk': (gpucc_node, 6),
                                 'axi_clk': (gcc_node, 13), 'memnoc_clk': (gcc_node, 34),
                                 'ahb_clk': (gpucc_node, 0), 'hub_clk': (gpucc_node, 20)})
        gbase, gbytes = gmu_reg[b'gmu']
        self.assertEqual({kgsl[b'kgsl_3d0_reg_memory'], kgsl[b'rscc'], kgsl[b'cx_misc'],
                          gmu_reg[b'gmu_ao_blk_dec0']}, set(windows) - {windows[1]})
        # Expose the GMU's TCM and HFI, not GPUCC's MMIO inside the larger stock debug window.
        gstart, glen = windows[1]
        self.assertTrue(gbase <= gstart <= 0x03d6d000 and 0x03d7e654 <= gstart + glen <= gbase + gbytes)
        self.assertTrue(all(s + n <= gpucc_base or s >= gpucc_base + gpucc_bytes for s, n in windows))
        ordered = sorted(windows)
        self.assertTrue(all(s + n <= after for (s, n), (after, _) in zip(ordered, ordered[1:])))
        self.assertEqual(irqs, [spi + 32 for node in (gpu, gmu) for kind, spi, flags in
                                struct.iter_unpack('>III', node['interrupts']) if (kind, flags) == (0, 4)])

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
            path = node[29:offset].split(b'\0', 1)[0].decode()
            self.assertEqual(len(node), offset + 20 * maps)
            named[path] = list(struct.iter_unpack('<5I', node[offset:]))
        self.assertEqual(set(named), {'\\_SB_.UFS0', '\\_SB_.URS0', '\\_SB_.GPU0'})
        app_smmu = named['\\_SB_.UFS0'][0][3]
        for path, xbl_name in (('\\_SB_.UFS0', 'NAMEDNODE_UFS_95MEM'), ('\\_SB_.URS0', 'NAMEDNODE_USB0')):
            route = self.xbl['/soc/iort/'+xbl_name+'/SIDMappings/SIDMappings_0']
            # A single mapping ignores the input and identifies the sole requester.
            self.assertEqual(named[path], [(0, 0, cell(route['OutputBase']), app_smmu, 1)])

        gpu = named['\\_SB_.GPU0']
        gpu_smmu = gpu[0][3]
        self.assertNotEqual(app_smmu, gpu_smmu)
        xbl_gpu = '/soc/iort/NAMEDNODE_GPU'
        expected = [(cell(n['InputBase']), cell(n['NumIDs']) - 1, cell(n['OutputBase']), gpu_smmu, cell(n['Flags']))
                    for p, n in self.xbl.items() if p.startswith(xbl_gpu + '/SIDMappings/')]
        self.assertEqual(gpu, sorted(expected))
        # In particular, the HAL's ACPI requester ID for the GMU is not its physical SID.
        handle, sid, mask = struct.unpack('>III', self.board['/soc/qcom,gmu@3d37000']['iommus'])
        self.assertEqual((handle, mask), (cell(self.board['/soc/kgsl-smmu@3da0000']['phandle']), 0))
        self.assertIn((0x03000005, 0, sid, gpu_smmu, 0), gpu)

        for offset, xbl_name, symbol in ((app_smmu, 'APPS_MMU500_SMMU_APP', 'apps_smmu'),
                                         (gpu_smmu, 'GPU_GFX_MMU500_SMMU_GFX', 'kgsl_smmu')):
            kind, smmu = nodes[offset]
            self.assertEqual(kind, 3)
            xbl = self.xbl['/soc/iort/' + xbl_name]
            dt = self.board[self.board['/__symbols__'][symbol].rstrip(b'\0').decode()]
            base, span = struct.unpack_from('<QQ', smmu, 16)
            self.assertEqual((base, span), struct.unpack('>II', dt['reg']))
            self.assertEqual(base, cell(xbl['BaseAddress']))
            self.assertEqual(struct.unpack_from('<II', smmu, 32), (cell(xbl['Model']), cell(xbl['Flags'])))
            global_off, count, context_off = struct.unpack_from('<III', smmu, 40)
            self.assertEqual(struct.unpack_from('<II', smmu, global_off),
                             (cell(xbl['NSGIRPT_GSIV']), cell(xbl['NSGIRPT_FLAGS'])))
            interrupts = [struct.unpack_from('<II', smmu, context_off + 8*i) for i in range(count)]
            self.assertEqual(interrupts, list(zip((v for (v,) in struct.iter_unpack('>I', xbl['ContextInterrupts'])),
                                                  (v for (v,) in struct.iter_unpack('>I', xbl['ContextInterruptFlags'])))))
        self.assertEqual(struct.unpack_from('<II', nodes[gpu_smmu][1], 8), (0, 0))  # Only wired GPU IRQs.
        smmu = nodes[app_smmu][1]
        msi_offset, = struct.unpack_from('<I', smmu, 12)
        in_id, _, out_id, its_offset, _ = struct.unpack_from('<5I', smmu, msi_offset)
        self.assertEqual(in_id, named['\\_SB_.UFS0'][0][2])
        self.assertEqual(out_id, cell(self.board['/soc/ufshc@1d84000']['msi-parent'][4:]))
        self.assertEqual(nodes[its_offset][0], 0)
