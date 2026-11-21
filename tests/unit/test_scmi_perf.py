"""Check the Windows SCMI client against a mailbox peer and the Piano CPU wiring."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class ScmiPerfTest(unittest.TestCase):
    def test_performance_protocol(self):
        src = ROOT / 'windows/drivers/cpupower'
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'scmi-perf'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', src,
                            src / 'scmi_channel.c', src / 'scmi_perf.c',
                            ROOT / 'tests/native/ScmiPerfTest.c', '-o', out], check=True)
            subprocess.run([out], check=True)

    def test_piano_shares_the_tx_slot_and_cpu_domains(self):
        dt = read_fdt((ROOT / 'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        handles = {node['phandle']: node for node in dt.values() if 'phandle' in node}
        controller = dt['/soc/qcom,scmi']
        tx, tx_channel, rx, rx_channel = struct.unpack('>4I', controller['mboxes'])
        self.assertEqual((tx, tx_channel, rx, rx_channel), (tx, 0, tx, 2))
        mbox = handles[tx.to_bytes(4, 'big')]
        self.assertEqual(mbox['compatible'], b'qcom,cpucp-v2\0')
        windows = dict(zip(mbox['reg-names'].rstrip(b'\0').split(b'\0'), struct.iter_unpack('>II', mbox['reg'])))
        self.assertEqual(windows[b'tx'], (0x17830000, 0x300))
        a, b = struct.unpack('>4s4s', controller['shmem'])
        self.assertEqual((handles[a]['reg'], handles[b]['reg']),
                         tuple(struct.pack('>QQ', base, 0x200) for base in (0x17b4e000, 0x17b4e200)))
        perf = dt['/soc/qcom,scmi/protocol@13']
        self.assertEqual(perf['reg'], bytes.fromhex('00000013'))
        for cluster, members in enumerate((6, 2)):
            paths = [n for p, n in dt.items() if p.startswith(f'/cpus/cpu-map/cluster{cluster}/core')]
            self.assertEqual(len(paths), members)
            for core in paths:
                self.assertEqual(handles[core['cpu']]['clocks'], perf['phandle'] + struct.pack('>I', cluster))
        self.assertIn('/soc/qcom,scmi/protocol@14', dt)
        self.assertIn('/soc/qcom,scmi/protocol@80', dt)


if __name__ == '__main__':
    unittest.main()
