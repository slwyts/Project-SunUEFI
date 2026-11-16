"""Actual native BL confirmation code; no input device or partition writes."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PianoBootloaderTests(unittest.TestCase):
    def test_actual_key_state_and_write_gate(self):
        with tempfile.TemporaryDirectory(prefix='piano-bootloader-') as folder:
            binary = Path(folder) / 'bootloader'
            subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                str(ROOT / 'tests/native/PianoBootloaderTest.c'),
                str(ROOT / 'android/native/piano-bootloader.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_shell_prompt_scoped_to_one_invocation(self):
        with tempfile.TemporaryDirectory(prefix='piano-bootloader-shell-') as folder:
            module = Path(folder)
            (module / 'bin').mkdir()
            tool = module / 'bin/piano-boot-repack'
            tool.write_text('#!/bin/sh\n[ "$1" = confirm-bootloader ] || exit 9\necho KEY_READER_STARTED\nexit "${PROMPT_RESULT:-0}"\n')
            tool.chmod(0o755)
            body = '''. "$1"
getprop() { echo 1; }
ui_print() { echo "$*"; }
MODPATH=$2
piano_check_bootloader || exit 3
[ "$PIANO_CONFIRMED_UNVERIFIED_BOOTLOADER" = 1 ]
'''
            command = ['sh', '-c', body, 'bootloader', str(ROOT / 'android/module/common.sh'), str(module)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    env={**os.environ, 'PROMPT_RESULT': '0'})
            self.assertEqual(result.returncode, 0)
            self.assertIn('三次音量下', result.stdout)
            self.assertIn('按音量上取消', result.stdout)
            self.assertLess(result.stdout.index('三次音量下'), result.stdout.index('KEY_READER_STARTED'))
            result = subprocess.run(command, capture_output=True, text=True,
                                     env={**os.environ, 'PROMPT_RESULT': '1'})
            self.assertEqual(result.returncode, 3)

    def test_custom_kernel_notice_does_not_treat_fingerprint_as_identity(self):
        with tempfile.TemporaryDirectory(prefix='piano-boot-source-') as folder:
            status = Path(folder) / 'status.json'
            command = ['sh', '-c', '''. "$1"
ui_print() { echo "$*"; }
piano_boot_source_warning "$2"
''', 'boot-source', str(ROOT / 'android/module/common.sh'), str(status)]
            status.write_text('{"boot_source_confirm_required":false}')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(result.stdout)
            status.write_text('{"boot_source_confirm_required":true}')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            self.assertIn('保留当前内核', result.stdout)
            self.assertIn('三次音量下', result.stdout)
            self.assertIn('音量上取消', result.stdout)


if __name__ == '__main__':
    unittest.main()
