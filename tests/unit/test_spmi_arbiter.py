"""Check the Piano SPMI interrupt lines and exercise the bus and IRQ operations."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt


class SpmiArbiterTest(unittest.TestCase):
    def test_transactions_and_interrupts(self):
        src = ROOT / 'windows/drivers/spmi'
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / 'spmi'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', src,
                            src/'arbiter.c', src/'interrupt.c', src/'gpio_input.c', ROOT/'tests/native/SpmiArbiterTest.c',
                            '-o', exe], check=True)
            subprocess.run([exe], check=True)

    def test_button_wiring(self):
        tree = read_fdt((ROOT/'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        handles = {n['phandle']: (p, n) for p, n in tree.items() if 'phandle' in n}
        path = '/soc/qcom,spmi@c42d000'
        bus = tree[path]
        self.assertEqual((int.from_bytes(bus['qcom,bus-id'], 'big'), int.from_bytes(bus['qcom,ee'], 'big')), (0, 0))
        regions = dict(zip(bus['reg-names'].rstrip(b'\0').decode().split('\0'),
                           struct.iter_unpack('>II', bus['reg']), strict=True))
        self.assertEqual(regions, {'core': (0xc400000, 0x3000), 'cnfg': (0xc42d000, 0x4000),
                                   'chnls': (0xc500000, 0x400000), 'obsrvr': (0xc440000, 0x80000),
                                   'intr': (0xc4c0000, 0x10000)})
        pdc, irq, flags = struct.unpack('>4sII', bus['interrupts-extended'])
        ranges = struct.iter_unpack('>III', handles[pdc][1]['qcom,pdc-ranges'])
        gsi, = (base + irq - pin + 32 for pin, base, size in ranges if pin <= irq < pin+size)
        self.assertEqual((gsi, flags), (778, 4))

        for name, bit, code in [('pwrkey', 7, 116), ('resin', 6, 114)]:
            node = tree[f'{path}/qcom,pmk8550@0/pon_hlos@1300/{name}']
            self.assertEqual(struct.unpack('>IIII', node['interrupts']), (0, 0x13, bit, 3))
            self.assertEqual(int.from_bytes(node['linux,code'], 'big'), code)
        up = tree['/soc/gpio_keys/vol_up']
        controller, pin, active_low = struct.unpack('>4sII', up['gpios'])
        gpio, gpio_node = handles[controller]
        self.assertTrue(gpio.startswith(path+'/qcom,pm8550@1/'))
        self.assertEqual((pin, active_low, int.from_bytes(up['debounce-interval'], 'big')), (6, 1, 32))
        base = int.from_bytes(gpio_node['reg'][:4], 'big')
        self.assertEqual((1 << 8) | ((base >> 8) + pin - 1), 0x18d)
        pad = tree[gpio+'/key_vol_up/key_vol_up_default']
        self.assertEqual((pad['pins'], pad['function'], int.from_bytes(pad['power-source'], 'big')),
                         (b'gpio6\0', b'normal\0', 1))
        self.assertIn('input-enable', pad)
        self.assertIn('bias-pull-up', pad)


if __name__ == '__main__':
    unittest.main()
