#!/usr/bin/env python3
"""Run the real GENI SPI FIFO engine against a host model of the serial engine"""
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
FLAGS = ["-std=gnu11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
INCLUDES = ["-I", str(ROOT / "uefi/core"), "-I", str(ROOT / "uefi/components/touch-product")]


class GeniSpiPioTest(unittest.TestCase):
    def test_engine_against_model(self):
        with tempfile.TemporaryDirectory(prefix="piano-geni-spi-") as out:
            executable = str(Path(out) / "spi")
            subprocess.run(["cc", *FLAGS, "-fsanitize=address,undefined",
                            "-I", str(INC), "-I", str(INC / HOST_PROCESSOR_BIND), *INCLUDES,
                            str(ROOT / "tests/native/PianoGeniSpiPioTest.c"),
                            str(ROOT / "uefi/components/touch-product/PianoGeniSpiPio.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True)

    @unittest.skipUnless(shutil.which("clang"), "clang is required for the AArch64 syntax check")
    def test_aarch64_syntax(self):
        subprocess.run(["clang", "--target=aarch64-windows-msvc", "-ffreestanding", "-fsyntax-only",
                        *FLAGS[1:], "-I", str(INC), "-I", str(INC / "AArch64"), *INCLUDES,
                        str(ROOT / "uefi/components/touch-product/PianoGeniSpiPio.c"),
                        str(ROOT / "uefi/components/touch-product/PianoTouchNvt.c"),
                        str(ROOT / "uefi/components/touch-product/PianoTouchMatrix.c")], check=True)


if __name__ == "__main__":
    unittest.main()
