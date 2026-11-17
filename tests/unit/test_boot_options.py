"""Run the firmware's standard load-option selection with fake UEFI services."""
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_product import simpleinit_boot_guid_header


class BootOptionsTests(unittest.TestCase):
    def test_mu_discovers_disks_without_boot_order(self):
        base = ROOT / 'upstream/Mu-Silicium/Mu_Basecore'
        relative = Path('MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c')
        patch = ROOT / 'patches/firmware/mu-basecore/cfb9372/0003-enumerate-empty-boot-order.patch'
        with tempfile.TemporaryDirectory(prefix='uefi-refresh-') as temporary:
            work = Path(temporary)
            target = work / relative
            target.parent.mkdir(parents=True)
            shutil.copyfile(base / relative, target)
            subprocess.run(['git', 'apply', str(patch)], cwd=work, check=True)
            source = target.read_text()
            start = source.index('VOID\nEFIAPI\nEfiBootManagerRefreshAllBootOption (')
            end = source.index('\n/**', start)
            (work / 'PianoActualBootOptionsRefresh.h').write_text(source[start:end])
            binary = work / 'refresh'
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(base / 'MdePkg/Include'), '-I' + str(base / 'MdePkg/Include/X64'),
                '-I' + str(base / 'MdeModulePkg/Include'), '-I' + str(work),
                str(ROOT / 'tests/native/PianoBootOptionsRefreshTest.c'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_boot_next_order_and_binary_options(self):
        with tempfile.TemporaryDirectory(prefix='uefi-options-') as temporary:
            work = Path(temporary)
            (work / 'PianoSimpleInitBootGuid.h').write_text(simpleinit_boot_guid_header())
            executable = work / 'boot-options'
            include = ROOT / 'upstream/Mu-Silicium/Mu_Basecore'
            flags = ['-I' + str(path) for path in (
                include / 'MdePkg/Include', include / 'MdePkg/Include/X64',
                include / 'MdeModulePkg/Include', ROOT / 'uefi/components/nv', work)]
            subprocess.run([
                'cc', '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', *flags,
                ROOT / 'tests/native/PianoBootOptionsTest.c', '-o', executable], check=True)
            subprocess.run([executable], check=True, env={
                **os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})


if __name__ == '__main__':
    unittest.main()
