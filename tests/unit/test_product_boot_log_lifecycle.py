"""Actual Core startup-display callbacks with host UEFI boundaries, no device."""
import platform
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
WS = ROOT / 'upstream/Mu-Silicium'


class ProductBootLogLifecycleTests(unittest.TestCase):
    def test_actual_core_display_event_lifetime(self):
        includes = [WS / 'Mu_Basecore/MdePkg/Include',
                    WS / 'Mu_Basecore/MdePkg/Include'/HOST_PROCESSOR_BIND,
                    WS / 'Mu_Basecore/MdeModulePkg/Include',
                    WS / 'Silicon/Qualcomm/QcomPkg/Include',
                    WS / 'Silicon/Silicium/SiliciumPkg/Include',
                    ROOT / 'uefi/components/guarded-read',
                    ROOT / 'uefi/components/product-handoff/Mu_Basecore/MdePkg/Include',
                    ROOT / 'uefi/components/nv',
                    ROOT / 'uefi/components/reset',
                    ROOT / 'uefi/components/pogo-product',
                    ROOT / 'uefi/components/touch-product',
                    ROOT / 'uefi/components/board-identity',
                    ROOT / 'uefi/components/rtc',
                    ROOT / 'uefi/handoff/early-memory']
        with tempfile.TemporaryDirectory(prefix='piano-core-bootlog-') as temporary:
            executable = Path(temporary) / 'lifecycle'
            mirror = Path(temporary) / 'includes'
            mirror.mkdir()
            app = ROOT / 'uefi/core'
            for path in app.rglob('*.h'):
                target = mirror / path.relative_to(app)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(path)
            (mirror / 'LateHandoff').symlink_to(ROOT / 'uefi/components/product-handoff', target_is_directory=True)
            (mirror / 'OsBoot').symlink_to(ROOT / 'uefi/components/os-boot', target_is_directory=True)
            (mirror / 'Components').mkdir()
            (mirror / 'Components/os-boot').symlink_to(ROOT / 'uefi/components/os-boot', target_is_directory=True)
            (mirror / 'uefi-app').symlink_to(app, target_is_directory=True)
            # Host build takes the non-AArch64 branches, EL1 DAIF access traps at EL0
            compiled = subprocess.run(['cc', '-U__aarch64__', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', '-fshort-wchar',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            *['-I' + str(path) for path in includes],
                            '-I' + str(mirror),
                            str(ROOT / 'tests/native/PianoProductBootLogLifecycleTest.c'),
                            '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())
