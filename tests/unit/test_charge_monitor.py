"""Exercise telemetry collection and the on-disk size limit without charger writes."""
import gzip
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'linux/bsp/common/usr/lib/piano/charge-monitor'
SPEC = spec_from_loader('charge_monitor', SourceFileLoader('charge_monitor', str(SOURCE)))
monitor = module_from_spec(SPEC)
SPEC.loader.exec_module(monitor)


def records(directory):
    result = []
    for path in sorted(directory.glob('samples-*.jsonl.gz')):
        result.extend(json.loads(line) for line in gzip.decompress(path.read_bytes()).splitlines())
    current = directory / 'samples.jsonl'
    if current.exists():
        result.extend(json.loads(line) for line in current.read_bytes().splitlines())
    return result


class ChargeMonitorTests(unittest.TestCase):
    def test_rotation_removes_oldest_records_and_survives_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            history = monitor.History(path, limit=8192, chunk=2048)
            for n in range(100):
                history.append({'n': n, 'value': os.urandom(250).hex()})
                self.assertLessEqual(sum(p.stat().st_size for p in path.iterdir()), 8192)
            saved = records(path)
            self.assertGreater(saved[0]['n'], 0)
            self.assertEqual([r['n'] for r in saved], list(range(saved[0]['n'], 100)))
            history.lock.close()
            with (path / 'samples.jsonl').open('ab') as stream:
                stream.write(b'{"interrupted":')
            history = monitor.History(path, limit=8192, chunk=2048)
            self.assertEqual(records(path), saved)
            history.append({'n': 100})
            self.assertEqual(records(path)[-1], {'n': 100})
            self.assertLessEqual(sum(p.stat().st_size for p in path.iterdir()), 8192)
            history.lock.close()

    def test_a_second_writer_cannot_corrupt_the_history(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            first = monitor.History(path)
            with self.assertRaises(BlockingIOError):
                monitor.History(path)
            first.lock.close()

    def test_telemetry_retains_values_and_selects_external_power_interval(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)

            def put(name, value):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(str(value) + '\n')

            put('proc/sys/kernel/random/boot_id', 'boot-identity')
            put('proc/loadavg', '0.1 0.2 0.3 1/100 1')
            usb = 'sys/class/power_supply/piano-mca-usb/'
            put(usb + 'online', 1)
            put(usb + 'type', 'USB')
            put(usb + 'voltage_now', 9000000)
            put(usb + 'current_now', 1000000)
            put(usb + 'charger_type', 'PD_VERIFY')
            put(usb + 'xiaomi/pd_verifed', 1)
            put(usb + 'xiaomi/verify_process', 1)
            cp = 'sys/class/power_supply/sc8541/'
            put(cp + 'online', 1)
            put(cp + 'voltage_now', 8900000)
            put(cp + 'current_now', 4000000)
            put(cp + 'charge_type', 'Standard')
            put('sys/class/power_supply/battery/current_now', -1000000)
            put('sys/class/power_supply/battery/voltage_now', 4000000)
            put('sys/class/thermal/thermal_zone7/type', 'piano-skin')
            put('sys/class/thermal/thermal_zone7/temp', 24500)
            put('sys/bus/iio/devices/iio:device3/name', 'board-adc')
            put('sys/bus/iio/devices/iio:device3/in_temp42_input', 21000)
            put('sys/bus/iio/devices/iio:device3/in_temp42_label', 'charger-therm')
            put('sys/class/thermal/cooling_device0/type', 'piano-charger')
            put('sys/class/thermal/cooling_device0/cur_state', 8)
            put('sys/kernel/debug/piano_mca/summary', 'pdos 3300-11000/5000')
            sampler = monitor.Sampler(root)
            sample = sampler.sample()
            self.assertEqual(sample['interval_s'], 2)
            self.assertEqual(sample['power_w']['input_estimate'], 44.6)
            self.assertEqual(sample['power_w']['battery'], -4.0)
            self.assertEqual(sample['thermal']['thermal_zone7']['temp'], 24500)
            self.assertEqual(sample['cooling']['cooling_device0']['cur_state'], 8)
            self.assertEqual(sample['power_supply']['sc8541']['charge_type'], 'Standard')
            self.assertIsNone(sample['firmware']['summary'])
            put(usb + 'xiaomi/verify_process', 0)
            self.assertIn('3300-11000', sampler.sample()['firmware']['summary'])
            put(usb + 'online', 0)
            put(cp + 'online', 0)
            self.assertEqual(sampler.sample()['interval_s'], 60)
            (root / (usb + 'online')).unlink()
            self.assertIsNone(sampler.external_power())


if __name__ == '__main__':
    unittest.main()
