"""Exercise APPS DRV2 and cross-check its assigned resources against the Piano DT."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class RpmhProviderTest(unittest.TestCase):
    def test_command_db_and_shared_clients(self):
        src = ROOT / 'windows/drivers/rpmh'
        cases = {
            'Provider': ['cmd_db', 'fabric', 'provider', 'vrm'],
            'Vrm': ['cmd_db', 'fabric', 'provider', 'vrm'],
            'Fabric': ['cmd_db', 'fabric'],
            'Rsc': ['rsc'],
        }
        with tempfile.TemporaryDirectory() as temp:
            for case, units in cases.items():
                with self.subTest(case=case):
                    out = Path(temp) / case
                    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', src,
                                    *(src / (name + '.c') for name in units),
                                    ROOT / f'tests/native/Rpmh{case}Test.c', '-o', out], check=True)
                    subprocess.run([out], check=True)

    def test_apps_channel_and_consumers(self):
        tree = read_fdt((ROOT / 'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        root, = (path for path, n in tree.items() if n.get('label') == b'apps_rsc\0')
        regs = list(struct.iter_unpack('>II', tree[root]['reg']))
        irqs = list(struct.iter_unpack('>III', tree[root]['interrupts']))
        drv = root + '/drv@2'
        index = int.from_bytes(tree[drv]['qcom,drv-id'], 'big')
        self.assertEqual(tree[root]['reg-names'].split(b'\0')[index], b'drv-2')
        self.assertEqual(regs[index], (0x16520000, 0x10000))
        self.assertEqual(irqs[index], (0, 5, 4))
        self.assertEqual(int.from_bytes(tree[drv]['qcom,tcs-offset'], 'big'), 0xd00)
        self.assertEqual(int.from_bytes(tree[drv]['qcom,tcs-distance'], 'big'), 0x2a0)
        types = list(struct.iter_unpack('>II', tree[drv+'/channel@0']['qcom,tcs-config']))
        self.assertEqual(types[:3], [(2, 3), (0, 2), (1, 2)])  # Active, sleep, wake.
        database, = (n for n in tree.values() if n.get('compatible') == b'qcom,cmd-db\0')
        self.assertEqual(struct.unpack('>QQ', database['reg']), (0x81c60000, 0x20000))
        self.assertIn('no-map', database)
        for name, resource in (('cxlvl', 'cx.lvl'), ('mxlvl', 'mx.lvl')):
            self.assertEqual(tree[drv+'/rpmh-regulator-'+name]['qcom,resource-name'], resource.encode() + b'\0')
        for path, names in (('/soc/ufshc@1d84000', ('ufs-ddr', 'cpu-ufs')),
                            ('/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000',
                             ('qup-core', 'qup-config', 'qup-memory'))):
            self.assertEqual(tree[path]['interconnect-names'].rstrip(b'\0').decode().split('\0'), list(names))

        hba = tree['/soc/ufshc@1d84000']
        self.assertEqual(hba['vccq-supply'], hba['qcom,vccq-proxy-vote-supply'])
        for pin, name, voltage, load, threshold in (('vcc','ldob17',2504000,1300000,10000),
                                                    ('vccq','ldod1',1200000,1200000,30000)):
            node, = (p for p, n in tree.items() if n.get('phandle') == hba[pin+'-supply'])
            rail, pmic = tree[node], tree[node.rsplit('/', 1)[0]]
            self.assertEqual(pmic['qcom,resource-name'].rstrip(b'\0').decode(), name)
            self.assertEqual(pmic['qcom,regulator-type'], b'pmic5-ldo\0')
            self.assertEqual(struct.unpack('>II', pmic['qcom,mode-threshold-currents']), (0, threshold))
            for prop in ('regulator-min-microvolt', 'regulator-max-microvolt', 'qcom,init-voltage'):
                self.assertEqual(int.from_bytes(rail[prop], 'big'), voltage)
            self.assertEqual(int.from_bytes(hba[pin+'-max-microamp'], 'big'), load)


if __name__ == '__main__':
    unittest.main()
