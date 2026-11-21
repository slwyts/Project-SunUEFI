"""Compare SE6 clock voting and pad selection with the factory DT and ClockDxe."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class PianoQupPowerTest(unittest.TestCase):
    def test_clock_votes_and_fixed_pads(self):
        src = ROOT / 'windows/drivers/spb'
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'piano-qup-power'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', src,
                            src/'gcc_vote.c', src/'piano_qup_clock.c', src/'piano_qup_power.c',
                            ROOT/'tests/native/PianoQupPowerTest.c', '-o', test], check=True)
            values = [int(n, 16) for n in subprocess.check_output([test], text=True).split()]
        vote, status, (cmd, cfg), masks, pins = values[0], values[1:6], values[6:8], values[8:13], values[13:]
        tree = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        handles = {props['phandle']: path for path, props in tree.items() if 'phandle' in props}
        se = tree['/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000']
        gcc = tree[handles[se['clocks'][:4]]]
        self.assertEqual(gcc['compatible'].split(b'\0')[0], b'qcom,sun-gcc')
        base, length = struct.unpack('>II', gcc['reg'])
        self.assertTrue(all(base <= address <= base + length - 4 for address in [vote, *status, cmd, cfg]))
        self.assertEqual(se['qcom,clk-freq-out'], struct.pack('>I', 1000000))
        active = se['pinctrl-0']
        self.assertEqual(len(active), 8)
        for i, gpio in enumerate(pins):
            pad = handles[active[i*4:i*4+4]]
            mux, config = tree[pad+'/mux'], tree[pad+'/config']
            self.assertEqual(mux['pins'], f'gpio{gpio}\0'.encode())
            self.assertEqual(mux['function'], f'qup1_se6_l{i}\0'.encode())
            self.assertEqual(config['drive-strength'], struct.pack('>I', 2))
            self.assertIn('bias-disable', config)
        self.assertEqual(len(pins), 2)

        firmware = (ROOT/'vendor/piano/native/ClockDxe.efi').read_bytes()
        def fields(name, domain=None):
            ref = struct.pack('<Q', firmware.index(name.encode() + b'\0'))
            rows = (struct.unpack_from('<8Q', firmware, p) for p in range(0, len(firmware)-63, 8)
                    if firmware[p:p+8] == ref)
            # Some names also occur in flat lists of strings. Only a clock
            # record references the GCC HAL domain in this field.
            row, = [row for row in rows if domain is None or row[3] == domain]
            return row
        # The firmware's register offsets use an internal GCC base. Derive
        # it from a GDSC also identified by the board, independently of SE6.
        supply = tree['/soc/ufshc@1d84000']['vdd-hba-supply']
        gdsc_base, _ = struct.unpack('>II', tree[handles[supply]]['reg'])
        gdsc = fields('gcc_ufs_phy_gdsc')
        hal_base = gdsc_base - gdsc[3]
        names = ('gcc_qupv3_wrap_1_m_ahb_clk', 'gcc_qupv3_wrap_1_s_ahb_clk',
                 'gcc_qupv3_wrap1_core_2x_clk', 'gcc_qupv3_wrap1_core_clk', 'gcc_qupv3_wrap1_s6_clk')
        for name, addr, mask in zip(names, status, masks):
            record = fields(name, gdsc[2])
            self.assertEqual((record[3], hal_base+record[4], hal_base+record[6], record[7]),
                             (gdsc[2], addr, vote, mask))
        source = fields(names[-1], gdsc[2])[1]
        hal, offset = struct.unpack_from('<QQ', firmware, source+32)
        self.assertEqual((hal, hal_base+offset, hal_base+offset+4), (gdsc[2], cmd, cfg))


if __name__ == '__main__':
    unittest.main()
