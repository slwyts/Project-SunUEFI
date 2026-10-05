"""Compile/run actual PianoPogo sources with real UEFI headers and ASan/UBSan."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PianoPogoTests(unittest.TestCase):
    def test_real_sources_native_uefi_abi(self):
        with tempfile.TemporaryDirectory(prefix="piano-pogo-test-") as directory:
            output = pathlib.Path(directory) / "pogo-test"
            include = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
            sources = [ROOT / "tests/PianoPogoReportTest.c"] + [ROOT / "bootprofiles/uefi-app" / name
                for name in ("PianoPogoReport.c", "PianoPogoInput.c", "PianoPogoI2c.c")]
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-fshort-wchar",
                "-fsanitize=address,undefined", "-I" + str(include), "-I" + str(include / "X64"),
                "-I" + str(ROOT / "bootprofiles/uefi-app"), *map(str, sources), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
