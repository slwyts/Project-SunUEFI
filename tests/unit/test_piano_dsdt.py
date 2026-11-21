"""Run Piano's compiled DSDT: processor idle, dual-role PnP, USB DSMs and UFS."""
import ast
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from uuid import UUID

ROOT = Path(__file__).resolve().parents[2]
DSDT = ROOT / 'uefi/components/acpi/Dsdt.asl'
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


def aml_result(text):
    """Decode ACPICA's external integer/string/buffer/package representations."""
    records = list(re.finditer(r'\[(Package|Integer|String|Buffer)\]\s+(.*)', text))
    def take(index):
        match = records[index]
        kind, heading = match.groups()
        pos = index + 1
        if kind == 'Package':
            n = int(re.search(r'Contains (\d+) Elements?', heading)[1])
            value = []
            for _ in range(n):
                item, pos = take(pos)
                value.append(item)
        elif kind == 'Integer':
            value = int(heading.split('=')[-1], 16)
        elif kind == 'String':
            value = ast.literal_eval(re.search(r'".*"', heading)[0])
        else:
            stop = records[pos].start() if pos < len(records) else len(text)
            data = text[match.start():stop]
            value = bytes.fromhex(' '.join(re.findall(r'\b[\da-f]+: ([\da-f ]+)//', data, re.I)))
            if len(value) != int(re.search(r'Length ([\da-fA-F]+)', heading)[1], 16):
                raise ValueError(f'Incomplete ACPICA buffer: {data}')
        return value, pos
    return take(0)[0]


def resources(blob):
    result = []
    off = 0
    while off < len(blob) and blob[off] != 0x79:
        kind, length = struct.unpack_from('<BH', blob, off)
        if kind == 0x86:
            result.append(('memory', *struct.unpack_from('<BII', blob, off + 3)))
        elif kind == 0x89:
            flags, count = struct.unpack_from('<BB', blob, off + 3)
            for irq, in struct.iter_unpack('<I', blob[off+5:off+5+4*count]):
                result.append(('irq', flags, irq))
        else:
            raise ValueError(f'Unexpected resource: {kind:#x}')
        off += 3+length
    if blob[off:] != b'\x79\x00':
        raise ValueError('Missing ACPI resource end tag')
    return result


class PianoDsdtTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        kit = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', ROOT / 'build/host-tools/usr')) / 'bin'
        iasl, cls.interpreter = [shutil.which(str(kit / n)) or shutil.which(n) for n in ('iasl', 'acpiexec')]
        if not iasl or not cls.interpreter:
            raise unittest.SkipTest('ACPICA compiler and interpreter are required')
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        prefix = Path(tmp.name) / 'piano'
        subprocess.run([iasl, '-we', '-p', prefix, DSDT], check=True, capture_output=True)
        cls.aml = prefix.with_suffix('.aml')
        cls.dt = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        cls.handles = {int.from_bytes(node['phandle'], 'big'): node for node in cls.dt.values() if 'phandle' in node}

    def evaluate(self, *items, table=None):
        commands = [x if x.startswith('set ') else 'evaluate '+x for x in items]
        result = subprocess.run([self.interpreter, table or self.aml], input='\n'.join(commands) + '\nquit\n',
                                check=True, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=20).stdout
        outcomes = re.findall(r'^Evaluation of \S+ returned object[^\n]*\n(.*?)(?=\n\n|\Z)', result, re.S | re.M)
        self.assertEqual(len(outcomes), sum(not x.startswith('set ') for x in items), result)
        return [aml_result(outcome) for outcome in outcomes]

    def test_preserved_methods_match_the_published_piano_aml(self):
        reference = DSDT.parent / 'pakala/DSDT.aml'
        container = r'\_SB.SYSM'
        calls = [r'set N \_SB.PEPI 1', container+'._LPI']
        for group, count in ((0, 6), (1, 2)):
            cluster = container+f'.CLS{group}'
            calls += [cluster+'._LPI']
            calls += [cluster+f'.CPU{i}._LPI' for i in range(count)]
        for role in ('USB0', 'UFN0'):
            dev = r'\_SB.URS0.'+role
            calls += [dev+'.RHUB.PRT1.'+prop for prop in ('_UPC','_PLD')]
            calls += [dev+'.'+prop for prop in ('_S0W','PHYC','CCVL')]
            guids = ['ce2ee385-00e6-48cb-9f05-2edb927c4899'] if role == 'USB0' else [
                'fe56cfeb-49d5-4378-a8a2-2978dbe54ad2','18de299f-9476-4fc9-b43b-8aeb713ed751']
            for guid in guids:
                for revision, function in ((0,0),(1,0),(0,1),(0,2),(0,3),(0,4),(1,7)):
                    calls.append(f'{dev}._DSM ({UUID(guid).bytes_le.hex(" ")}) {revision} {function} []')
        self.assertEqual(self.evaluate(*calls), self.evaluate(*calls, table=reference))

    def test_cover_reports_the_live_gpio_and_notifies_windows(self):
        lid, gpio = r'\_SB.LID0', r'\_SB.TLMM'
        control, pin, polarity = struct.unpack('>III', self.dt['/soc/xiaomi_hall/lid_hall']['gpios'])
        self.assertIn(b'qcom,sun-tlmm', self.handles[control]['compatible'].split(b'\0'))
        self.assertEqual(polarity, 1)  # Active low: closed is zero in ACPI.
        hid, event = self.evaluate(lid+'._HID', gpio+'._AEI')
        self.assertEqual(hid, 0x0d0cd041)  # EISA PNP0C0D
        self.assertEqual((event[0], event[4], event[7]), (0x8c, 0, 5))  # GPIO interrupt, both edges
        pin_offset, = struct.unpack_from('<H', event, 14)
        self.assertEqual(struct.unpack_from('<H', event, pin_offset), (pin,))
        edge = gpio+f'._E{pin:02X}'
        commands = '; '.join('evaluate '+q for q in
                             (gpio+'._REG 8 0', lid+'._LID', gpio+'._REG 8 1', lid+'._LID', edge))
        for level in (0, 1):
            output = subprocess.check_output([self.interpreter, '-dt', '-r', '-fv', str(level), '-b', commands,
                                              self.aml], text=True, stderr=subprocess.STDOUT, timeout=15)
            states = re.findall(r'Evaluation of \S+\._LID returned object[^\n]*\n\s*\[Integer\] = (\w+)', output)
            self.assertEqual([int(s, 16) for s in states], [1, level], output)
            self.assertRegex(output.split('Evaluating '+edge)[-1], r'Notify on \[LID0\].*Value 0x80')

    def test_processor_containers_and_arm_idle_entries(self):
        sysm = r'\_SB.SYSM'
        self.assertEqual(self.evaluate(sysm+'._HID', sysm+'._UID'), ['ACPI0010', 0x100000])
        self.assertEqual(self.dt['/soc/psci']['method'], b'smc\0')
        self.assertEqual(self.dt['/cpus/idle-states']['entry-method'], b'psci\0')
        all_uids = {}
        for group in (0, 1):
            container = sysm+f'.CLS{group}'
            self.assertEqual(self.evaluate(container+'._HID', container+'._UID'), ['ACPI0010', 0x10+group])
            cores = sorted((int(path.rsplit('core', 1)[1]), self.handles[int.from_bytes(v['cpu'], 'big')])
                           for path, v in self.dt.items() if path.startswith(f'/cpus/cpu-map/cluster{group}/core'))
            names = [container+f'.CPU{i}.'+key for i, _ in cores for key in ('_HID','_UID','_STA','_LPI')]
            data = iter(self.evaluate(*names))
            for _, cpu in cores:
                hid, uid, sta, lpi = [next(data) for _ in range(4)]
                all_uids[uid] = int.from_bytes(cpu['reg'], 'big')
                self.assertEqual((hid, sta, cpu['enable-method']), ('ACPI0007', 15, b'psci\0'))
                self.assertEqual(lpi[:3], [0, 0, 2])
                expected = int.from_bytes(self.handles[int.from_bytes(cpu['cpu-idle-states'], 'big')]
                                          ['arm,psci-suspend-param'], 'big')
                for state, ffh in zip(lpi[3:], (0xffffffff, expected), strict=True):
                    self.assertEqual((len(state), state[2], state[3]), (10, 1, 0))
                    self.assertEqual(state[6], struct.pack('<BHBBBBQBB', 0x82, 12, 0x7f, 32, 0, 3, ffh, 0x79, 0))
        mpidrs = sorted(int.from_bytes(v['reg'], 'big') for v in self.dt.values() if v.get('device_type') == b'cpu\0')
        self.assertEqual(sorted(all_uids.items()), list(enumerate(mpidrs)))

    def test_platform_idle_depends_on_the_pep(self):
        methods = [r'\_SB.SYSM._LPI', r'\_SB.SYSM.CLS0._LPI', r'\_SB.SYSM.CLS1._LPI']
        off = self.evaluate(*methods)
        on = self.evaluate(r'set N \_SB.PEPI 1', *methods)
        for disabled, enabled, names in zip(off, on, (['platform.SS1','platform.SS2','platform.DRIPS'],
                                                      ['Cluster0.CL4','Cluster0.CL5'], ['Cluster1.CL4','Cluster1.CL5']),
                                            strict=True):
            self.assertEqual(enabled[:3], disabled[:3])
            self.assertEqual(enabled[2], len(names))
            for a, b, name in zip(disabled[3:], enabled[3:], names, strict=True):
                self.assertEqual((a[2], b[2], b[9]), (0, 1, name))
                self.assertEqual(a[:2]+a[3:], b[:2]+b[3:])

    def test_usb_role_resources_and_one_physical_connector(self):
        urs = r'\_SB.URS0'
        role = [urs+'.USB0', urs+'.UFN0']
        wrapper = self.dt['/soc/ssusb@a600000']
        ctl = self.dt['/soc/ssusb@a600000/dwc3@a600000']
        [declared, cache, irq_host, irq_device] = self.evaluate(
            urs+'._CRS', urs+'._CCA', role[0]+'._CRS', role[1]+'._CRS')
        self.assertEqual(resources(declared), [('memory', 1, *struct.unpack_from('>II', wrapper['reg']))])
        self.assertEqual(bool(cache), 'dma-coherent' in ctl)
        self.assertEqual((ctl['dr_mode'], ctl['maximum-speed']), (b'otg\0', b'super-speed\0'))
        cells = iter(k[0] for k in struct.iter_unpack('>I', wrapper['interrupts-extended']))
        wake = {}
        for name in wrapper['interrupt-names'].rstrip(b'\0').split(b'\0'):
            provider = self.handles[next(cells)]
            args = [next(cells) for _ in range(int.from_bytes(provider['#interrupt-cells'], 'big'))]
            if 'qcom,pdc-ranges' in provider:
                pin, trigger = args
                [(base, target)] = [(a,b) for a,b,n in struct.iter_unpack('>III',provider['qcom,pdc-ranges'])
                                    if a <= pin < a+n]
                spi = target + pin - base
            else:
                kind, spi, trigger = args
                self.assertEqual(kind, 0)
            self.assertIn(trigger, (1,4))
            wake[name] = ('irq', 0x19 | (2 if trigger == 1 else 0), spi+32)
        kind, spi, trigger = struct.unpack('>III', ctl['interrupts'])
        self.assertEqual((kind, trigger), (0, 4))
        common = [('irq', 9, spi+32), wake[b'pwr_event_irq']]
        self.assertEqual(resources(irq_device), common)
        self.assertEqual(resources(irq_host), common + [wake[x] for x in (b'ss_phy_irq',b'dp_hs_phy_irq',b'dm_hs_phy_irq')])
        results = self.evaluate(*(dev+prop for dev in role for prop in
                                ('._ADR','._S0W','.RHUB._ADR','.RHUB.PRT1._ADR','.RHUB.PRT1._UPC','.RHUB.PRT1._PLD')))
        self.assertEqual(results[:5], [0,3,0,1,[1,9,0,0]])
        self.assertEqual(results[6:11], [1,3,0,1,[1,9,0,0]])
        self.assertEqual(results[5], results[11])
        [pld] = results[5]
        self.assertEqual((len(pld), pld[0] & 127, pld[8] & 1), (20,2,1))

    def test_usb_device_specific_functions_and_ufs_child(self):
        host, device = r'\_SB.URS0.USB0', r'\_SB.URS0.UFN0'
        queries = []
        def dsm(path, guid, revision, function):
            buf = UUID(guid).bytes_le.hex(' ')
            return f'{path}._DSM ({buf}) {revision} {function} []'
        for path, guid, answer in ((host, 'ce2ee385-00e6-48cb-9f05-2edb927c4899', (b'\x1d',0,0,2)),
                                   (device,'fe56cfeb-49d5-4378-a8a2-2978dbe54ad2',(b'\x03',0x20)),
                                   (device,'18de299f-9476-4fc9-b43b-8aeb713ed751',(b'\x03',0x39))):
            indexes = (0,2,3,4) if path == host else (0,1)
            queries.extend(zip((dsm(path,guid,0,i) for i in indexes), answer, strict=True))
            queries += [(dsm(path,guid,1,0), b'\x01'), (dsm(path,'00000000-0000-0000-0000-000000000000',0,0), b'\x00')]
        self.assertEqual(self.evaluate(*(query for query,_ in queries)), [answer for _,answer in queries])
        ufs = r'\_SB.UFS0'
        hid, cid, cls, sub, addr, rmv, crs = self.evaluate(*(ufs+'.'+prop for prop in
                                                       ('_HID','_CID','_CLS','_SUB','DEV0._ADR','DEV0._RMV','_CRS')))
        self.assertEqual((hid, cid, cls, sub, addr, rmv), ('QCOM24A5',r'PCI\CC_010901',[1,9,1],'MTP08750',8,0))
        hci = self.dt['/soc/ufshc@1d84000']
        phy = self.dt['/soc/ufsphy_mem@1d80000']
        self.assertEqual(resources(crs), [('memory',1,*struct.unpack_from('>II',hci['reg'])),
                                          ('memory',1,*struct.unpack('>II',phy['reg'])),
                                          ('irq',1,32+struct.unpack('>III',hci['interrupts'])[1])])


if __name__ == '__main__':
    unittest.main()
