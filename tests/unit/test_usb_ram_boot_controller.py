"""Actual Controller wrapper source, new isolated entry; no prod/device edits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
INCLUDES=[ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include",
          ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64",
          ROOT/"upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include"]


class UsbRamBootControllerTests(unittest.TestCase):
    def test_actual_controller_default_off_and_sixteen_retirement_cases(self):
        with tempfile.TemporaryDirectory(prefix="ram-boot-controller-")as directory:
            for enabled in(0,1):
                exe=Path(directory)/("controller-"+str(enabled))
                cmd=["cc","-std=gnu11","-fshort-wchar","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-misleading-indentation",
                    "-g","-fsanitize=address,undefined","-fno-pie","-no-pie",f"-DPIANO_USB_RAM_BOOT={enabled}"]
                for include in INCLUDES:cmd += ["-I",str(include)]
                cmd += [str(ROOT/"tests/native/PianoUsbRamBootControllerTest.c"),"-o",str(exe)]
                build=subprocess.run(cmd,text=True,capture_output=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
                run=subprocess.run([str(exe)],text=True,capture_output=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1"})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())


if __name__=="__main__":unittest.main()
