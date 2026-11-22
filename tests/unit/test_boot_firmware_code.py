"""Run the product's SEC section protection against PI firmware-volume fixtures."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
DRIVER = ROOT / 'uefi/components/product-support/Drivers/BootFirmwareCodeDxe'


class BootFirmwareCodeTests(unittest.TestCase):
    def test_only_in_place_firmware_text_becomes_read_only_and_executable(self):
        cpu = 'AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp)/'sections'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fshort-wchar','-DNO_MSABI_VA_FUNCS',
                            '-I',BASE,'-I',BASE/cpu,DRIVER/'FirmwareVolume.c',
                            ROOT/'tests/native/BootFirmwareCodeTest.c','-o',exe],check=True)
            subprocess.run([exe],check=True)
