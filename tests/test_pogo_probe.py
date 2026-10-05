"""Actual probe/backend/guard host cases and production ARM64 assembly check."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
QCOM = ROOT / "upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"
APP = ROOT / "bootprofiles/uefi-app"

class PogoProbeTests(unittest.TestCase):
    def test_actual_probe(self):
        with tempfile.TemporaryDirectory(prefix="piano-pogo-probe-") as out:
            for enabled in (0, 1):
                binary = str(Path(out) / f"probe-{enabled}")
                subprocess.run(["cc", "-std=gnu11", "-fshort-wchar", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    f"-DPIANO_POGO_PROBE_EXPERIMENT={enabled}", "-I", str(INC), "-I", str(INC / "X64"), "-I", str(QCOM),
                    str(ROOT / "tests/PianoPogoProbeTest.c"), str(APP / "PianoGeniI2cPio.c"), "-o", binary], check=True)
                for case in ([0] if not enabled else range(24)):
                    subprocess.run([binary, str(case)], check=True, capture_output=True)
            # A real object assembly check, not fsyntax-only: validates labels,
            # guard PC stores, LDR32/resume, DAIF and PAR inline instructions.
            subprocess.run([str(ROOT / "build/host-tools/usr/bin/clang"), "--target=aarch64-windows-msvc",
                "-fshort-wchar", "-ffreestanding", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-DPIANO_POGO_PROBE_EXPERIMENT=1", "-I", str(INC), "-I", str(INC / "AArch64"), "-I", str(QCOM),
                "-c", str(APP / "PianoPogoProbe.c"), "-o", str(Path(out) / "probe.obj")], check=True)

if __name__ == "__main__":
    unittest.main()
