"""Four actual translation units, host EFI mocks only; no device or prepare."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
APP = ROOT / "uefi/core"

class LaunchIntegrationTest(unittest.TestCase):
    def test_actual_download_to_launch_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="sunuefi-launch-integration-") as out:
            binary = str(Path(out) / "integration")
            subprocess.run([
                "cc", "-std=gnu11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-misleading-indentation", "-Wno-deprecated-declarations", "-g", "-fsanitize=address,undefined",
                "-fno-pie", "-no-pie", "-fno-omit-frame-pointer",
                "-I", str(INC), "-I", str(INC / "X64"),
                "-I", str(ROOT / "upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include"),
                str(ROOT / "tests/native/PianoFastbootLaunchIntegrationTest.c"),
                *[str(APP / name) for name in (
                    "PianoFastboot.c", "PianoFastbootDownloadBlob.c",
                    "PianoFastbootBoot.c", "PianoFastbootLaunch.c")],
                str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),"-lcrypto", "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

if __name__ == "__main__":
    unittest.main()
