#!/usr/bin/env python3
"""Keyboard MCU bootrom and runtime byte formats, including the pinned image"""
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
FIRMWARE = ROOT / "upstream/piano-firmware-current/firmware/nanosic/MCU_Upgrade.bin"


class PogoMcuTest(unittest.TestCase):
    def test_formats(self):
        with tempfile.TemporaryDirectory(prefix="piano-pogo-mcu-") as out:
            executable = str(Path(out) / "mcu")
            subprocess.run(["cc", "-std=gnu11", "-fshort-wchar", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-fsanitize=address,undefined",
                            "-I", str(INC), "-I", str(INC / HOST_PROCESSOR_BIND), "-I", str(ROOT / "uefi/components/pogo-product"),
                            str(ROOT / "tests/native/PianoPogoMcuTest.c"),
                            str(ROOT / "uefi/components/pogo-product/PianoPogoMcu.c"), "-o", executable], check=True)
            subprocess.run([executable, *([str(FIRMWARE)] if FIRMWARE.is_file() else [])], check=True)

    def test_product_embeds_pinned_firmware(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        import prepare_product
        if not FIRMWARE.is_file():
            self.skipTest("piano-firmware checkout is not present")
        with tempfile.TemporaryDirectory(prefix="piano-pogo-fw-") as out:
            record = prepare_product.prepare_pogo_firmware(ROOT, Path(out))
            header = (Path(out) / "PianoPogoMcuFirmware.h").read_text()
        self.assertTrue(0 < record["ram_code_bytes"] <= 0x10000)
        self.assertIn(record["sha256"], header)
        self.assertIn("mPianoPogoMcuImageHeader", header)
        self.assertIn("mPianoPogoMcuCode", header)


if __name__ == "__main__":
    unittest.main()
