"""Select the actual kernel launcher and execute it without input/device access."""
import os
import json
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
import stage_piano_ram_hardware as hardware


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
        subprocess.run(['git', 'apply',
                        str(ROOT / 'patches/linux/7.3/0124-input-nt36532e-pen-centroid.patch')],
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

    def test_target_calibration_staging_uses_bsp_and_preserves_other_files(self):
        root = ROOT / 'build/distros'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='pen-calibration-', dir=root) as directory:
            target = Path(directory)
            unrelated = target / 'usr/lib/firmware/unrelated.bin'
            unrelated.parent.mkdir(parents=True)
            unrelated.write_bytes(b'keep firmware')
            rows = hardware.stage_pen_calibration(target)
            self.assertEqual(len(rows), 4)
            self.assertEqual(unrelated.read_bytes(), b'keep firmware')
            _, _, record, _ = runtime.pen_input_frontend(self.kernel, target)
            self.assertEqual(record['mode'], 'kernel')
            for name in runtime.PEN_CALIBRATION_PATHS:
                self.assertTrue((target / name).is_symlink())
            hardware.stage_pen_calibration(target)
            self.assertEqual(unrelated.read_bytes(), b'keep firmware')

    def test_target_calibration_preflight_rejects_alias_before_writing(self):
        root = ROOT / 'build/distros'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='pen-calibration-', dir=root) as directory:
            target = Path(directory)
            alias = target / 'usr/share/piano/pen/calibration/piano-csot.ini'
            alias.parent.mkdir(parents=True)
            alias.symlink_to(self.work / 'outside')
            with self.assertRaisesRegex(ValueError, 'guest symlink'):
                hardware.stage_pen_calibration(target)
            self.assertFalse((alias.parent / 'piano-boe.ini').exists())
            self.assertFalse((self.work / 'outside').exists())

    def test_actual_runtime_entries_accept_distinct_firmware_root(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/build_release_helpers.py'),
                                 '--firmware-root', str(self.rootfs), '--sysroot', '/', '--help'],
                                capture_output=True, text=True, check=True)
        self.assertIn('--firmware-root', result.stdout)
        result = subprocess.run([sys.executable, str(ROOT / 'tools/build_piano_runtime_helpers.py'),
                                 '--firmware-root', str(self.rootfs), '--sysroot', '/', '--help'],
                                capture_output=True, text=True, check=True)
        self.assertIn('--firmware-root', result.stdout)
        args = runtime.parser().parse_args([
            '--output', str(self.work / 'output'), '--sysroot', '/',
            '--firmware-root', str(self.rootfs)])
        self.assertEqual(args.sysroot, Path('/'))
        self.assertEqual(args.firmware_root, self.rootfs)

    @unittest.skipUnless(shutil.which('aarch64-linux-gnu-gcc') and
                         shutil.which('aarch64-linux-gnu-g++') and
                         shutil.which('qemu-aarch64-static'), 'ARM64 cross toolchain and QEMU required')
    def test_real_pen_compile_uses_separate_target_firmware_root(self):
        compiler = shutil.which('aarch64-linux-gnu-gcc')
        sysroot = Path(subprocess.check_output([compiler, '-print-sysroot'], text=True).strip())
        self.assertNotEqual(sysroot, self.rootfs)
        self.assertFalse((sysroot / next(iter(runtime.PEN_CALIBRATION_PATHS))).exists())
        output = self.work / 'compiled'
        output.mkdir()
        files, record = runtime.build_pen_core(
            output, [compiler, '--sysroot=' + str(sysroot)], sysroot,
            emulator=shutil.which('qemu-aarch64-static'), kernel=self.kernel,
            firmware_root=self.rootfs)
        self.assertEqual(record['input']['mode'], 'kernel')
        self.assertTrue(runtime.verify_elf(output / files['usr/bin/piano-pen-offline']['file']))
        self.assertEqual(json.loads((output / 'pen-manifest.json').read_text())['input']['calibration'],
                         record['input']['calibration'])
        launcher = (output / files['usr/lib/piano/touch-input']['file']).read_bytes()
        self.assertNotIn(b'--pen-input', launcher)


if __name__ == '__main__':
    unittest.main()
