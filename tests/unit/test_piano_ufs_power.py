"""Verify the two GCC power controls against Piano's DT and ClockDxe."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class PianoUfsPowerTest(unittest.TestCase):
    def test_source_registers_and_power_sequences(self):
        src = ROOT / 'windows/drivers/ufs'
        with tempfile.TemporaryDirectory() as temp:
            test = Path(temp) / 'ufs-power'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', src, src/'piano_power.c',
                            ROOT/'tests/native/PianoUfsPowerTest.c', '-o', test], check=True)
            addresses = [int(n, 16) for n in subprocess.check_output([test], text=True).split()]
        board = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        firmware = (ROOT/'vendor/piano/native/ClockDxe.efi').read_bytes()
        expected, offsets = [], []
        for device, supply, parent in (('/soc/ufshc@1d84000', 'vdd-hba-supply', 'cxlvl'),
                                       ('/soc/ufsphy_mem@1d80000', 'vdd-phy-gdsc-supply', 'mxlvl')):
            phandle = board[device][supply]
            node, = (n for n in board.values() if n.get('phandle') == phandle)
            address, size = struct.unpack('>II', node['reg'])
            self.assertEqual((size, node['compatible']), (4, b'qcom,gdsc\0'))
            self.assertIn('qcom,support-cfg-gdscr', node)
            self.assertIn('qcom,retain-regs', node)
            upstream, = (k for k, v in board.items() if v.get('phandle') == node['parent-supply'])
            self.assertIn('rpmh-regulator-' + parent + '/', upstream)
            expected.append(address)
            name = firmware.index(node['regulator-name'])
            record, = (i for i in range(0, len(firmware)-8, 8)
                       if firmware[i:i+8] == struct.pack('<Q', name))
            offsets.append(struct.unpack_from('<Q', firmware, record+24)[0])
        self.assertEqual(addresses, expected)
        self.assertNotEqual(addresses[0], addresses[1])
        self.assertEqual(expected[1]-expected[0], offsets[1]-offsets[0])


if __name__ == '__main__':
    unittest.main()
