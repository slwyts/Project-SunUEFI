"""Execute the actual resident Core entry with host-only service boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WS = ROOT / 'upstream/Mu-Silicium'


class ProductCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='piano-product-core-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / 'core'
        mirror = directory / 'includes'
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
        includes = [WS / 'Mu_Basecore/MdePkg/Include', WS / 'Mu_Basecore/MdePkg/Include/X64',
                    WS / 'Mu_Basecore/MdeModulePkg/Include',
                    WS / 'Silicon/Qualcomm/QcomPkg/Include', WS / 'Silicon/Silicium/SiliciumPkg/Include',
                    ROOT / 'uefi/components/guarded-read',
                    ROOT / 'uefi/components/product-handoff/Mu_Basecore/MdePkg/Include',
                    ROOT / 'uefi/components/nv', ROOT / 'uefi/components/reset',
                     ROOT / 'uefi/handoff/early-memory', ROOT / 'uefi/components/pogo-product',
                     ROOT / 'uefi/components/touch-product', mirror]
        includes += [ROOT / 'uefi/components/board-identity', ROOT / 'uefi/components/rtc']
        compiled = subprocess.run(
            ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
             '-fshort-wchar', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
             *['-I' + str(path) for path in includes],
             str(ROOT / 'tests/native/PianoProductCoreTest.c'), '-o', str(cls.executable)],
            capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_standard_efi_requires_actual_active_uki_descendant(self):
        self.run_case('efi-admission')

    def test_failed_uki_reports_original_error_without_menu_retry(self):
        self.run_case('uki-load-failure')

    def test_error_screen_uses_only_live_unretired_usb_diagnostics(self):
        self.run_case('error-diagnostics')
