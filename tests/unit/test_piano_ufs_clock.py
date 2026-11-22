"""Cross-check the board clock provider against real ClockDxe records and the factory DT."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class PianoUfsClockTest(unittest.TestCase):
    def test_shared_gcc_translation_power_and_removal(self):
        gcc = ROOT / 'windows/drivers/gcc'
        ufs = ROOT / 'windows/drivers/ufs'
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'sm8750-gcc'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', ROOT / 'tests/fixtures/gcc', '-I', gcc, '-I', ufs,
                            gcc / 'fixed_pll.c', ufs / 'piano_clock.c', ufs / 'piano_base_clock.c',
                            ufs / 'piano_power.c', ROOT / 'tests/native/Sm8750GccTest.c',
                            '-o', exe], check=True)
            subprocess.run([exe], check=True, timeout=10)

    def test_bus_roots_branches_reference_and_rollback(self):
        unit=ROOT/'windows/drivers/ufs'
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'ufs-base'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',unit,unit/'piano_base_clock.c',
                            ROOT/'tests/native/PianoUfsBaseClockTest.c','-o',exe],check=True)
            addresses=[int(n,16) for n in subprocess.check_output([exe],text=True).split()]
        fw=(ROOT/'vendor/piano/native/ClockDxe.efi').read_bytes()
        def word(p):
            return struct.unpack_from('<Q',fw,p)[0]
        def record(name):
            p=fw.index(name.encode()+b'\0')
            row,=(j for j in range(0,len(fw)-8,8) if word(j)==p)
            return row
        dt=read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        xbl=read_fdt((ROOT/'vendor/piano/dtb/xbl-config.dtb').read_bytes())['tree']
        gdsc=next(n for n in dt.values() if n.get('regulator-name')==b'gcc_ufs_phy_gdsc\0')
        offset=struct.unpack('>II',gdsc['reg'])[0]-word(record('gcc_ufs_phy_gdsc')+24)
        names=('gcc_ufs_phy_axi_clk','gcc_aggre_ufs_phy_axi_clk','gcc_ufs_phy_ahb_clk',
               'gcc_ufs_phy_unipro_core_clk','gcc_ufs_phy_ice_core_clk','gcc_ufs_phy_phy_aux_clk')
        self.assertEqual(addresses[8:14],[offset+word(record(n)+32) for n in names])
        rcgs=[offset+word(word(record(names[i])+8)+40) for i in (0,3,4,5)]
        self.assertEqual(addresses[:8],[r+d for r in rcgs for d in (0,4)])
        timing={n['clk_name'].rstrip(b'\0').decode():struct.unpack('>6I',n['config'])
                for n in xbl.values() if 'clk_name' in n and len(n.get('config',b''))==24}
        for i in (0,3,4): self.assertEqual(timing[names[i]][2:],(100000000,201500000,403000000,403000000))
        h=dt['/soc/ufshc@1d84000'];phy=dt['/soc/ufsphy_mem@1d80000']
        rpmh=next(n for n in dt.values() if n.get('compatible')==b'qcom,sun-rpmh-clk\0')['phandle']
        tcsr=next(n for n in dt.values() if b'qcom,sun-tcsrcc' in n.get('compatible',b''))
        self.assertEqual(addresses[-1],struct.unpack('>II',tcsr['reg'])[0]+0x1000)
        for n,k,owner,idx in ((h,'ref_clk',rpmh,4),(phy,'ref_clk_src',rpmh,27),(phy,'qref_clk',tcsr['phandle'],1)):
            name=n['clock-names'].rstrip(b'\0').decode().split('\0').index(k)
            self.assertEqual(n['clocks'][8*name:8*name+8],owner+struct.pack('>I',idx))
        for name,hz,l,alpha in (('gcc_gpll0',600000000,31,0x4000),('gcc_gpll4',806000000,41,0xfaaa)):
            config=word(record(name)+24)
            self.assertEqual((struct.unpack_from('<I',fw,config)[0],*struct.unpack_from('<II',fw,config+28)),(hz,l,alpha))

    def test_inherited_cold_and_retry_against_stock_clock_records(self):
        unit = ROOT / 'windows/drivers/ufs'
        with tempfile.TemporaryDirectory() as path:
            exe = Path(path) / 'piano-ufs-clock'
            subprocess.run(['cc','-std=c17','-Wall','-Wextra','-Werror','-I',unit,unit/'piano_clock.c',
                            ROOT/'tests/native/PianoUfsClockTest.c','-o',exe], check=True)
            bcr, *lane = [int(n,16) for n in subprocess.check_output([exe],text=True).split()]
        board = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        xbl = read_fdt((ROOT/'vendor/piano/dtb/xbl-config.dtb').read_bytes())['tree']
        host = board['/soc/ufshc@1d84000']
        phy = board['/soc/ufsphy_mem@1d80000']
        handle, reset_id = struct.unpack('>II',host['resets'])
        gcc, = (n for n in board.values() if n.get('phandle') == handle.to_bytes(4,'big'))
        self.assertEqual((gcc['compatible'].split(b'\0')[0],reset_id,host['reset-names']),
                         (b'qcom,sun-gcc',23,b'rst\0'))
        self.assertEqual(struct.unpack('>II',phy['resets']), (int.from_bytes(host['phandle'],'big'),0))
        base, length = struct.unpack('>II',gcc['reg'])
        gdsc_handle = int.from_bytes(host['vdd-hba-supply'],'big')
        gdsc, = (n for n in board.values() if n.get('phandle') == gdsc_handle.to_bytes(4,'big'))
        gdsc_address, _ = struct.unpack('>II',gdsc['reg'])
        self.assertTrue(all(base <= address <= base+length-4 for address in [bcr,*lane]))
        self.assertNotIn(gdsc_address,[bcr,*lane])
        firmware = (ROOT/'vendor/piano/native/ClockDxe.efi').read_bytes()
        def fields(name):
            string = firmware.index(name+b'\0')
            refs = [i for i in range(0, len(firmware)-8, 8) if firmware[i:i+8] == struct.pack('<Q',string)]
            self.assertEqual(len(refs),1)
            return refs[0]
        def qword(at):
            return struct.unpack_from('<Q',firmware,at)[0]
        gdsc_record = fields(b'gcc_ufs_phy_gdsc')
        domain_base = qword(gdsc_record+16)
        # ClockDxe's register offsets use its internal HAL base, not the
        # start of Android's larger GCC region. Anchor both to the same GDSC.
        hal_base = gdsc_address - qword(gdsc_record+24)
        name_root=xbl['/__symbols__']['ufs'].rstrip(b'\0').decode()+'/'
        xbl_clocks={p['clk_name'].rstrip(b'\0') for k,p in xbl.items() if k.startswith(name_root) and 'clk_name' in p}
        for i,label in enumerate(('rx_symbol_0','rx_symbol_1','tx_symbol_0')):
            name=f'gcc_ufs_phy_{label}_clk'.encode()
            self.assertIn(name,xbl_clocks)
            record = fields(name)
            parent_record = qword(record+8)
            self.assertEqual((qword(record+24),qword(parent_record+32)),(domain_base,domain_base))
            self.assertEqual([hal_base+qword(record+32),hal_base+qword(parent_record+40)],lane[2*i:2*i+2])
            self.assertEqual(hal_base+qword(record+40),bcr)


if __name__ == '__main__':
    unittest.main()
