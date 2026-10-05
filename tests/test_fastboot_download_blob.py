"""Actual current-pool ownership adapter tests; no USB or firmware build."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"


class DownloadBlobTests(unittest.TestCase):
    def test_actual_pool_and_command_source(self):
        with tempfile.TemporaryDirectory(prefix="sunuefi-download-blob-") as out:
            binary = str(Path(out) / "ownership")
            subprocess.run([
                "cc", "-std=gnu11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-I", str(INC), "-I", str(INC / "X64"),
                "-I", str(ROOT / "upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include"),
                str(ROOT / "tools/test_fastboot_download_blob.c"), "-lcrypto", "-o", binary
            ], check=True)
            subprocess.run([binary], check=True)
            subprocess.run([
                str(ROOT / "build/host-tools/usr/bin/clang"), "--target=aarch64-windows-msvc",
                "-fshort-wchar", "-ffreestanding", "-fsyntax-only", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-parameter", "-I", str(INC),
                "-I", str(INC / "AArch64"),
                str(ROOT / "bootprofiles/uefi-app/PianoFastbootDownloadBlob.c")
            ], check=True)


if __name__ == "__main__":
    unittest.main()
