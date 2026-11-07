#!/usr/bin/env python3
"""Run the real GENI I2C FIFO engine against a host model of the serial engine"""
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


class GeniPioTest(unittest.TestCase):
    def test_engine_against_model(self):
        with tempfile.TemporaryDirectory(prefix="piano-geni-pio-") as out:
            executable = str(Path(out) / "pio")
            subprocess.run(["cc", *FLAGS, "-fsanitize=address,undefined",
                            "-I", str(INC), "-I", str(INC / HOST_PROCESSOR_BIND), "-I", str(ROOT / "uefi/core"),
                            str(ROOT / "tests/native/PianoGeniI2cPioTest.c"),
                            str(ROOT / "uefi/core/PianoGeniI2cPio.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True)

    @unittest.skipUnless(shutil.which("clang"), "clang is required for the AArch64 syntax check")
    def test_aarch64_syntax(self):
        subprocess.run(["clang", "--target=aarch64-windows-msvc", "-ffreestanding", "-fsyntax-only",
                        *FLAGS[1:], "-I", str(INC), "-I", str(INC / "AArch64"),
                        str(ROOT / "uefi/core/PianoGeniI2cPio.c")], check=True)


if __name__ == "__main__":
    unittest.main()
