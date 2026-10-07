"""Run the actual collector with bounded host-only ADB response fixtures."""
import contextlib
import io
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / 'tools/collect_uefi_ramlog.py'


class RamLogCollectorTests(unittest.TestCase):
    def collect(self, console):
        with tempfile.TemporaryDirectory(prefix='piano-ramlog-') as directory:
            root = Path(directory)
            (root / 'tools').mkdir()
            script = root / 'tools/collect_uefi_ramlog.py'
            script.write_bytes(SOURCE.read_bytes())
            records = root / 'private/analysis'
            records.mkdir(parents=True)
            (records / 'stage0-test-1.json').write_text(json.dumps(
                {'fastboot_boot': {'exit_code': 0}}))

            def adb(command, **kwargs):
                self.assertEqual(command[:3], ['adb', '-s', 'fixture'])
                if command[3:] == ['shell', 'getprop', 'sys.boot_completed']:
                    return subprocess.CompletedProcess(command, 0, '1\n', '')
                if command[3:] == ['exec-out', "su -c 'cat /sys/fs/pstore/console-ramoops-0'"]:
                    return subprocess.CompletedProcess(command, 0, console, b'')
                if command[3:] == ['shell', 'ls', '-1', '/sys/fs/pstore']:
                    return subprocess.CompletedProcess(command, 0, '', '')
                self.fail('Unexpected collector device command: ' + repr(command))

            with patch('sys.argv', [str(script), '--serial', 'fixture', '--test-id', '1']), \
                    patch('subprocess.run', side_effect=adb), contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(str(script), run_name='__main__')
            result = records / 'ramlog-test-1'
            self.assertEqual((result / 'console.txt').read_bytes(), console)
            return json.loads((result / 'manifest.json').read_text()), (result / 'uefi.txt').read_text()

    def test_preserve_overwritten_prefix_without_inventing_session_or_memory_proof(self):
        console = (b'SUNUEFI_UFS_DMA_COMPLETE command=BLOCKIO_READ status=Success\r\n'
                   b'image: icon @bootitem-piano-setup not found\r\n'
                   b'uefikeyboard: PIANO_KEY_EVENT scan=0x0 unicode=0xd\r\n')
        report, text = self.collect(console)
        self.assertEqual(report['uefi_log_scope'], 'wrapped-uefi-tail-with-product-ui-events')
        self.assertTrue(report['startup_prefix_overwritten'])
        self.assertTrue(report['simpleinit_menu_events_found'])
        self.assertFalse(report['uefi_marker_found'])
        self.assertFalse(report['product_core_ready'])
        self.assertIn('BLOCKIO_READ', text)

    def test_intact_cold_product_marker_remains_distinct(self):
        report, text = self.collect(b'Firmware Version fixture\nSUNUEFI_EARLY_SMEM status=Success\nPIANO_PRODUCT_CORE_READY\n')
        self.assertTrue(report['uefi_marker_found'])
        self.assertTrue(report['product_core_ready'])
        self.assertFalse(report['startup_prefix_overwritten'])
        self.assertTrue(text.startswith('Firmware Version'))

    def test_isolated_input_text_is_not_a_firmware_session(self):
        report, text = self.collect(b'PIANO_KEY_EVENT without a firmware DMA record\n')
        self.assertEqual(report['uefi_log_scope'], 'none')
        self.assertFalse(report['uefi_marker_found'])
        self.assertEqual(text, '')
