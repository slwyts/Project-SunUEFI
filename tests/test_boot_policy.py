"""Actual static provider/supervisor/common FV loader; no prepare/device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
APP=ROOT/"bootprofiles/uefi-app"
class BootPolicyTests(unittest.TestCase):
    def test_actual_supervisor(self):
        with tempfile.TemporaryDirectory(prefix="piano-boot-policy-") as out:
            binary=str(Path(out)/"policy")
            flags=["-I",str(INC),"-I",str(INC/"X64"),"-I",str(ROOT/"upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"),"-I",str(ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include")]
            subprocess.run(["cc","-std=gnu11","-fshort-wchar","-g","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                "-fsanitize=address,undefined","-fno-pie","-no-pie",*flags,str(ROOT/"tests/PianoBootPolicyTest.c"),
                str(APP/"PianoBootPolicy.c"),str(APP/"PianoFvApplication.c"),"-o",binary],check=True)
            for case in range(64):
                result=subprocess.run([binary,str(case)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,f"case {case}: {result.stdout}\n{result.stderr}")
            subprocess.run([str(ROOT/"build/host-tools/usr/bin/clang"),"--target=aarch64-windows-msvc","-fshort-wchar","-ffreestanding",
                "-fsyntax-only","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-I",str(INC),"-I",str(INC/"AArch64"),
                "-I",str(ROOT/"upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"),"-I",str(ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include"),str(APP/"PianoBootPolicy.c"),str(APP/"PianoFvApplication.c")],check=True)
if __name__=="__main__":unittest.main()
