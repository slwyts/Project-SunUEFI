"""Actual UEFI C, no hardware. Architecture object inspection is ARM64."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
INCLUDE=ROOT/"upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include"
SOURCE=ROOT/"uefi/core/PianoHighRamProbe.c"


class HighRamProbeTests(unittest.TestCase):
    def test_actual_c_all_default_gates_and_readonly_future_pattern_paths(self):
        with tempfile.TemporaryDirectory(prefix="high-probe-host-")as directory:
            for probe,pattern in ((0,0),(0,1),(1,0),(1,1)):
                exe=Path(directory)/f"probe-{probe}-{pattern}"
                command=["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-misleading-indentation","-g","-fshort-wchar","-fsanitize=address,undefined","-fno-pie","-no-pie",
                    f"-DPIANO_HIGH_RAM_PROBE_EXPERIMENT={probe}",f"-DPIANO_HIGH_RAM_PATTERN_EXPERIMENT={pattern}",
                    "-I"+str(INCLUDE),"-I"+str(INCLUDE/"X64"),"-I"+str(ROOT/"uefi/core"),
                    str(ROOT/"tests/native/PianoHighRamProbeTest.c"),str(SOURCE),"-o",str(exe)]
                build=subprocess.run(command,capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stdout+build.stderr)
                run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1"})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                self.assertIn("PianoHighRamProbe actual C PASS",run.stdout);print(probe,pattern,run.stdout.strip())

    def test_actual_aarch64_instructions_default_absence_and_no_mapping_writes(self):
        host=ROOT/"build/host-tools/usr";environment={**os.environ,"LD_LIBRARY_PATH":str(host/"lib")}
        with tempfile.TemporaryDirectory(prefix="high-probe-arm64-")as directory:
            for probe,pattern in ((0,0),(1,0),(1,1)):
                obj=Path(directory)/f"probe-{probe}-{pattern}.o"
                command=[str(host/"bin/clang"),"--target=aarch64-none-elf","-std=c11","-ffreestanding","-fshort-wchar","-O2","-Wall","-Wextra","-Werror",
                    f"-DPIANO_HIGH_RAM_PROBE_EXPERIMENT={probe}",f"-DPIANO_HIGH_RAM_PATTERN_EXPERIMENT={pattern}",
                    "-I"+str(INCLUDE),"-I"+str(INCLUDE/"AArch64"),"-I"+str(ROOT/"uefi/core"),"-c",str(SOURCE),"-o",str(obj)]
                build=subprocess.run(command,env=environment,capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stdout+build.stderr)
                asm=subprocess.run([str(host/"bin/llvm-objdump"),"-d",str(obj)],env=environment,check=True,capture_output=True,text=True).stdout.lower()
                self.assertEqual("at\ts1e1r"in asm,bool(probe));self.assertEqual("at\ts1e1w"in asm,bool(probe and pattern))
                if probe:
                    self.assertIn("mrs",asm);self.assertIn("par_el1",asm);self.assertIn("daif",asm)
                for register in("ttbr0_el1","tcr_el1","mair_el1","sctlr_el1"):
                    self.assertNotRegex(asm,r'msr\s+'+register)
                symbols=subprocess.run([str(host/"bin/llvm-readelf"),"-s",str(obj)],env=environment,check=True,capture_output=True,text=True).stdout
                for name in("AllocatePages","AllocatePool","SetMemorySpaceAttributes","AddMemorySpace","DmaMap","gBS","gDS"):
                    self.assertNotIn(name,symbols)
                if not probe:self.assertNotIn("\tblr\t",asm)
                print("AArch64 actual source",probe,pattern,"AT/PAR/DAIF and no mapping/allocator/DMA writes PASS")


if __name__=="__main__":unittest.main()
