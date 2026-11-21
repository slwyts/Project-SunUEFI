"""Compare the Windows UFS PHY programming trace with this board's XBL tables."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class UfsPlatformTest(unittest.TestCase):
    def test_takeover_and_original_two_lane_calibration(self):
        ufs = ROOT / 'windows/drivers/ufs'
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'ufs-platform'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', ufs, ufs / 'platform.c',
                            ROOT / 'tests/native/UfsPlatformTest.c', '-o', executable], check=True)
            trace = subprocess.check_output([executable, 'calibration'], text=True)

        tree = read_fdt((ROOT / 'vendor/piano/dtb/xbl-config.dtb').read_bytes())['tree']
        node = tree['/__symbols__']['ufs'].rstrip(b'\0').decode()
        self.assertEqual(int.from_bytes(tree[node]['RefClock'], 'big'), 38_400_000)
        for setting in ('init_speed_params', 'perf_speed_params'):
            mode = tree[f'{node}/{setting}']
            self.assertEqual([mode[k] for k in ('EnableHighSpeed', 'NumGears', 'NumLanes', 'Rate')],
                             [b'\x01', b'\x05', b'\x02', b'\x02'])
        expected = []
        for table in ('MphyInitTable', 'MphyLaneInitTable', 'MphyInitEndTable'):
            entries = list(struct.iter_unpack('>HHH', tree[f'{node}/ufs_phy_cfg'][table]))
            self.assertEqual(entries.pop(), (0x8000, 0, 0))
            for mode, offset, value in entries:
                if mode:
                    self.assertIn((mode, offset, value), ((1, 0x420, 0x40), (2, 0x420, 0x41)))
                    if mode == 1:  # Both XBL profiles select rate B.
                        continue
                expected.append((offset, value))

        writes = [tuple(int(v, 16) for v in line.split()) for line in trace.splitlines()]
        # Power and reset are separate timed operations, not calibration data.
        controls = {0x400, 0x404, 0x408}
        self.assertEqual([item for item in writes if item[0] in controls],
                         [(0x404, 1), (0x408, 1), (0x408, 0), (0x400, 1)])
        self.assertEqual([item for item in writes if item[0] not in controls],
                         [item for item in expected if item[0] not in controls])


if __name__ == '__main__':
    unittest.main()
