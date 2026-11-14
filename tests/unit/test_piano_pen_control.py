"""Select the actual kernel launcher and execute it without input/device access."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_runtime_helpers as runtime
import package_bsp


class PianoPenControlTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='piano-pen-control-')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.source, self.build, self.rootfs = (self.work / name for name in ('kernel', 'build', 'rootfs'))
        self.build.mkdir()
        self.rootfs.mkdir()
        # These are actual source files from the stored kernel patch, not a fake decoder.
        subprocess.run(['git', 'apply', '--include=drivers/input/touchscreen/nt36532e/nt36xxx-pen*',
                        '--include=include/linux/input/xiaomi-pen.h',
                        str(ROOT / 'patches/linux/7.3/0123-input-nt36532e-pen.patch')],
                       cwd=self.work, check=True, capture_output=True)
        self.source.mkdir()
        for name in runtime.PEN_KERNEL_SOURCE_FILES:
            source = self.work / name
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(source, target)
        (self.build / '.config').write_text(
            'CONFIG_TOUCHSCREEN_NT36532E_SPI=m\nCONFIG_HID_XIAOMI=m\nCONFIG_FW_LOADER=y\n')
        (self.build / 'Module.symvers').write_text(''.join(
            f'0x00000001\t{name}\tdrivers/hid/hid-xiaomi\tEXPORT_SYMBOL_GPL\t\n'
            for name in ('xiaomi_pen_register_notifier', 'xiaomi_pen_unregister_notifier')))
        _, rows = package_bsp.payload([])
        for row, content in rows:
            if row['path'].startswith(('usr/lib/firmware/novatek/piano-', 'usr/share/piano/pen/calibration/')):
                target = self.rootfs / row['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                if row['type'] == 'symlink':
                    target.symlink_to(content.decode())
                else:
                    target.write_bytes(content)
        self.kernel = {'source': str(self.source), 'build': str(self.build), 'release': '7.3.3-piano'}

    def test_matched_kernel_launcher_execs_only_bluez_even_before_ble_input_exists(self):
        self.assertFalse((self.rootfs / 'sys/class/input').exists())
        data, source, record, _ = runtime.pen_input_frontend(self.kernel, self.rootfs)
        self.assertEqual(source.name, 'piano-pen-control')
        self.assertEqual(record['mode'], 'kernel')
        self.assertFalse(record['legacy_input_enabled'])
        self.assertNotIn(b'/proc/', data)
        self.assertNotIn(b'piano-touch-view', data)
        self.assertNotIn(b'--pen-input', data)
        helper = self.work / 'pen-bluetooth'
        helper.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TRACE"\nexit 23\n')
        helper.chmod(0o755)
        frontend = self.work / 'touch-input'
        text = data.decode().replace('/usr/lib/piano/pen-bluetooth', str(helper))
        text = text.replace('/usr/lib/firmware/', str(self.rootfs / 'usr/lib/firmware') + '/')
        frontend.write_text(text)
        trace = self.work / 'trace'
        result = subprocess.run(['sh', str(frontend)], env=dict(os.environ, TRACE=str(trace)),
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 23)
        self.assertIn('--follow', trace.read_text())
        self.assertNotIn('--stationary-device', trace.read_text())

    def test_incomplete_kernel_or_missing_calibration_refuses_mixed_runtime(self):
        (self.source / runtime.PEN_KERNEL_SOURCE_FILES[0]).unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            runtime.pen_input_frontend(self.kernel, self.rootfs)

    def test_missing_calibration_or_unreachable_hid_does_not_fall_back_to_uinput(self):
        calibration = self.rootfs / next(iter(runtime.PEN_CALIBRATION_PATHS))
        calibration.unlink()
        with self.assertRaisesRegex(ValueError, 'calibration'):
            runtime.pen_input_frontend(self.kernel, self.rootfs)
        (self.build / '.config').write_text(
            'CONFIG_TOUCHSCREEN_NT36532E_SPI=y\nCONFIG_HID_XIAOMI=m\nCONFIG_FW_LOADER=y\n')
        with self.assertRaisesRegex(ValueError, 'cannot reach'):
            runtime.pen_input_frontend(self.kernel, self.rootfs)

    def test_missing_notifier_build_cannot_select_kernel_input(self):
        (self.build / 'Module.symvers').write_text('')
        with self.assertRaisesRegex(ValueError, 'matched kernel pen/HID modules'):
            runtime.pen_input_frontend(self.kernel, self.rootfs)

    def test_old_kernel_keeps_legacy_launcher(self):
        empty = self.work / 'old-kernel'
        empty.mkdir()
        self.kernel['source'] = str(empty)
        data, source, record, _ = runtime.pen_input_frontend(self.kernel, self.rootfs)
        self.assertEqual(source.name, 'piano-touch-input')
        self.assertEqual(record['mode'], 'userspace')
        self.assertIn(b'--pen-input', data)


if __name__ == '__main__':
    unittest.main()
