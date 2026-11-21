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

        for supply, peer, source, name, kind, vmin, vmax, init, load, threshold in (
            ('qcom,vddp-ref-clk','/soc/spf_core_platform/lpass-cdc/wcd939x-codec',
             'cdc-vdd-px','ldoi2','pmic5-ldo',1200000,1200000,1200000,100,30000),
            ('qcom,vccq-parent','/soc/bt_peach','qcom,bt-vdd-rfa1p25',
             'smpi7','pmic5-ftsmps',1224000,1340000,1256000,210000,200000),
        ):
            ref = hba[supply + '-supply']
            self.assertEqual(ref, tree[peer][source + '-supply'])
            path, = (key for key, node in tree.items() if node.get('phandle') == ref)
            regulator, hardware = tree[path], tree[path.rsplit('/',1)[0]]
            self.assertEqual(hardware['qcom,resource-name'].rstrip(b'\0').decode(), name)
            self.assertEqual(hardware['qcom,regulator-type'].rstrip(b'\0').decode(), kind)
            self.assertEqual(struct.unpack('>II',hardware['qcom,supported-modes']), (1,4))
            self.assertEqual(struct.unpack('>II',hardware['qcom,mode-threshold-currents']), (0,threshold))
            self.assertEqual(tuple(int.from_bytes(regulator[v],'big') for v in
                                   ('regulator-min-microvolt','regulator-max-microvolt','qcom,init-voltage')),
                             (vmin,vmax,init))
            self.assertEqual(int.from_bytes(hba[supply+'-max-microamp'],'big'),load)
        self.assertEqual(hba['qcom,vccq-parent-supply'],tree['/soc/qcom,cnss-peach@b0000000']['vdd-wlan-rfa2-supply'])

    def test_phy_supplies_and_shared_voltage_limits(self):
        tree = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        phy = tree['/soc/ufsphy_mem@1d80000']
        self.assertEqual(int.from_bytes(phy['vdda-phy-min-microvolt'],'big'),912000)
        for supply, resource, load, voltages in (
            ('vdda-phy','ldoj1',213000,(880000,920000,912000)),
            ('vdda-pll','ldog3',18300,(1200000,1256000,1200000)),
            ('vdda-qref','ldoi3',64500,(880000,912000,880000)),
            ('vdda-refgen','ldof1',206000,(880000,920000,880000)),
        ):
            key, = (k for k,v in tree.items() if v.get('phandle')==phy[supply+'-supply'])
            rail,pmic=tree[key],tree[key.rsplit('/',1)[0]]
            self.assertEqual(pmic['qcom,resource-name'].rstrip(b'\0').decode(),resource)
            self.assertEqual(pmic['qcom,regulator-type'],b'pmic5-ldo\0')
            self.assertEqual(struct.unpack('>II',pmic['qcom,supported-modes']),(2,4))
            self.assertEqual(struct.unpack('>II',pmic['qcom,mode-threshold-currents']),(0,30000))
            self.assertEqual(tuple(int.from_bytes(rail[p],'big') for p in
                                   ('regulator-min-microvolt','regulator-max-microvolt','qcom,init-voltage')),voltages)
            self.assertEqual(int.from_bytes(phy[supply+'-max-microamp'],'big'),load)
        for device, s, pin, uv in (
            ('/soc/qcom,mdss_dsi_ctrl0@ae94000','vdda-pll','vdda-1p2',1200000),
            ('/soc/qcom,mdss_dsi_phy0@ae95500','vdda-qref','vdda-0p9',880000),
        ):
            self.assertEqual(phy[s+'-supply'],tree[device][pin+'-supply'])
            constraint, = (n for k,n in tree.items() if k.startswith(device+'/') and
                           n.get('qcom,supply-name') == pin.encode()+b'\0')
            self.assertEqual([int.from_bytes(constraint['qcom,supply-'+limit+'-voltage'],'big')
                              for limit in ('min','max')],[uv,uv])
        self.assertEqual(phy['vdda-refgen-supply'],tree['/soc/hsphy@88e3000']['vdd_refgen-supply'])
        self.assertEqual(phy['vdda-pll-supply'],tree['/soc/ssphy@88e8000']['core-supply'])


if __name__ == '__main__':
    unittest.main()
