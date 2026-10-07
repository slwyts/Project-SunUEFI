"""Actual app/core/adapter/launch/probe, fixed built fixture; no device/prepare."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2];APP=ROOT/"uefi/core"
spec=importlib.util.spec_from_file_location("ram_probe_builder_app",ROOT/"tools/build_ram_boot_probe.py")
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
SHA="e42f0ae416c1843ed1dcdaa87301f9a34b093becec9c829d46ce03388038d2b9"


class UsbRamBootAppTests(unittest.TestCase):
    def test_actual_core_to_carrier_to_launch_and_return_probe(self):
        includes=[ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include",ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64",ROOT/"upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include"]
        with tempfile.TemporaryDirectory(prefix="ram-boot-app-host-")as directory:
            folder=Path(directory)/"fixture";builder.build(folder);fixture=folder/"PianoRamBootProbe.efi"
            raw=fixture.read_bytes();self.assertEqual(len(raw),3584);self.assertEqual(hashlib.sha256(raw).hexdigest(),SHA)
            for enabled in(0,1):
                exe=Path(directory)/("app-"+str(enabled))
                cmd=["cc","-std=gnu11","-fshort-wchar","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-misleading-indentation",
                    "-g","-fsanitize=address,undefined","-fno-pie","-no-pie",f"-DPIANO_USB_RAM_BOOT={enabled}"]
                for include in includes:cmd += ["-I",str(include)]
                cmd += ["-Wno-deprecated-declarations",str(ROOT/"tests/native/PianoUsbRamBootAppTest.c"),*[str(APP/name)for name in("PianoFastboot.c","PianoFastbootBoot.c","PianoFastbootDownloadBlob.c","PianoFastbootLaunch.c","PianoRamBootProbe.c")],str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),"-lcrypto","-o",str(exe)]
                build=subprocess.run(cmd,text=True,capture_output=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
                run=subprocess.run([str(exe),str(fixture)],text=True,capture_output=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1"})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())


if __name__=="__main__":unittest.main()
