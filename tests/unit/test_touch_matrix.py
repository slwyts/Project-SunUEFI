#!/usr/bin/env python3
"""THP matrix decoder on synthetic payloads"""
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND = 'AArch64' if platform.machine() in ('aarch64', 'arm64') else 'X64'
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"


class TouchMatrixTest(unittest.TestCase):
    def test_decoder(self):
        with tempfile.TemporaryDirectory(prefix="piano-touch-matrix-") as out:
            executable = str(Path(out) / "matrix")
            subprocess.run(["cc", "-std=gnu11", "-fshort-wchar", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-fsanitize=address,undefined", "-I", str(INC), "-I", str(INC / HOST_PROCESSOR_BIND),
                            "-I", str(ROOT / "uefi/components/touch-product"),
                            str(ROOT / "tests/native/PianoTouchMatrixTest.c"),
                            str(ROOT / "uefi/components/touch-product/PianoTouchMatrix.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
