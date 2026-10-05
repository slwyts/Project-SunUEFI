"""Compile real Mu PEI/HOB source with real Mu types and a hardware-only shim."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class CmaHobTests(unittest.TestCase):
    def test_real_mu_pei_hob_contract(self):
        candidate=ROOT/'artifacts/dram/cma-contract-candidate/MemoryMapLib.c'
        self.assertEqual(hashlib.sha256(candidate.read_bytes()).hexdigest(),'70aaab84738198b18a78b0f11f715f15fa4c11295f30befe8ab82a98e693b7cc')
        includes=[BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include']
        with tempfile.TemporaryDirectory(prefix='piano-cma-hob-') as temp:
            exe=Path(temp)/'cma-hob'
            cmd=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',str(ROOT/'tests/PianoCmaPcdShim.h')]
            for inc in includes:cmd+=['-I',str(inc)]
            cmd+=[str(ROOT/'tests/PianoCmaHobTest.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),'-Wl,--gc-sections','-o',str(exe)]
            build=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],cwd=ROOT,text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('Real Mu MemoryPeim/AddHob + real PrePiHobLib',run.stdout)
            print(run.stdout.strip())
if __name__=='__main__':unittest.main()
