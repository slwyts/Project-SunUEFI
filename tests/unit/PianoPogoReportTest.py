"""Compile/run actual PianoPogo sources with real UEFI headers and ASan/UBSan."""
import sys
import pathlib
import subprocess
import tempfile
import unittest
from core_source import core_include_flags, core_source


ROOT = pathlib.Path(__file__).resolve().parents[2]


class PianoPogoTests(unittest.TestCase):
    def test_real_sources_native_uefi_abi(self):
        with tempfile.TemporaryDirectory(prefix="piano-pogo-test-") as directory:
            output = pathlib.Path(directory) / "pogo-test"
            include = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
            sources = [ROOT / "tests/native/PianoPogoReportTest.c"] + [core_source(name, ROOT)
                for name in ("PianoPogoReport.c", "PianoPogoInput.c", "PianoPogoI2c.c")]
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-fshort-wchar",
                "-fsanitize=address,undefined", "-I" + str(include), "-I" + str(include / "X64"),
                *core_include_flags(ROOT), *map(str, sources), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
