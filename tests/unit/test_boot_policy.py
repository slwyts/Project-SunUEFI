"""Actual static provider/supervisor/common FV loader; no prepare/device."""
import sys
import platform
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from core_source import core_source, core_include_flags


ROOT=Path(__file__).resolve().parents[2]
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
INC=ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
APP=ROOT/"uefi/core"
PUMP=ROOT/'uefi/components/product-pump/Mu_Basecore/MdePkg/Include'
class BootPolicyTests(unittest.TestCase):
    def test_actual_supervisor(self):
        with tempfile.TemporaryDirectory(prefix="piano-boot-policy-") as out:
            binary=str(Path(out)/"policy")
            flags=[*core_include_flags(ROOT),"-I",str(ROOT/'uefi/components/nv'),"-I",str(PUMP),"-I",str(INC),"-I",str(INC/HOST_PROCESSOR_BIND),"-I",str(ROOT/"upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"),"-I",str(ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include")]
            # Host build takes the non-AArch64 branches, EL1 DAIF access traps at EL0
            subprocess.run(["cc","-U__aarch64__","-std=gnu11","-fshort-wchar","-g","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                "-fsanitize=address,undefined","-fno-pie","-no-pie",*flags,str(ROOT/"tests/native/PianoBootPolicyTest.c"),
                str(core_source('PianoBootPolicy.c', ROOT)),str(core_source('PianoFvApplication.c', ROOT)),"-o",binary],check=True)
            for case in range(153):
                result=subprocess.run([binary,str(case)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,f"case {case}: {result.stdout}\n{result.stderr}")
            subprocess.run([str(TOOLCHAIN/"bin/clang"),"--target=aarch64-windows-msvc","-fshort-wchar","-ffreestanding",
                "-fsyntax-only","-Wall","-Wextra","-Werror","-Wno-unused-parameter",*core_include_flags(ROOT),"-I",str(ROOT/'uefi/components/nv'),"-I",str(PUMP),"-I",str(INC),"-I",str(INC/"AArch64"),
                "-I",str(ROOT/"upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"),"-I",str(ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include"),str(core_source('PianoBootPolicy.c', ROOT)),str(core_source('PianoFvApplication.c', ROOT))],check=True)
if __name__=="__main__":unittest.main()
