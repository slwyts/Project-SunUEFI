"""Four actual translation units, host EFI mocks only; no device or prepare."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
APP = ROOT / "bootprofiles/uefi-app"

class LaunchIntegrationTest(unittest.TestCase):
    def test_actual_download_to_launch_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="sunuefi-launch-integration-") as out:
            binary = str(Path(out) / "integration")
            subprocess.run([
                "cc", "-std=gnu11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-g", "-fsanitize=address,undefined",
                "-fno-pie", "-no-pie", "-fno-omit-frame-pointer",
                "-I", str(INC), "-I", str(INC / "X64"),
                "-I", str(ROOT / "upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include"),
                str(ROOT / "tests/PianoFastbootLaunchIntegrationTest.c"),
                *[str(APP / name) for name in (
                    "PianoFastboot.c", "PianoFastbootDownloadBlob.c",
                    "PianoFastbootBoot.c", "PianoFastbootLaunch.c")],
                "-lcrypto", "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

if __name__ == "__main__":
    unittest.main()
