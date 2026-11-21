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
