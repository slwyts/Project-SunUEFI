import platform
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
class ProductBootObjectsTests(unittest.TestCase):
 def test_actual_cached_consumer_duplicate_crc_bounds_lifetime(self):
  with tempfile.TemporaryDirectory(prefix='cold-dxe-')as td:
   exe=Path(td)/'replay';cmd=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie']
   fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
   for p in (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/HOST_PROCESSOR_BIND,ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',ROOT/'uefi/handoff/early-memory',fdt):cmd+=['-I',str(p)]
   cmd+=[str(ROOT/'tests/native/PianoProductBootObjectsTest.c'),str(ROOT/'uefi/core/PianoProductBootObjects.c'),str(ROOT/'uefi/handoff/early-memory/PianoColdBootObjectsContract.c')]
   # The Gunyah DT update links the shared libfdt host adapter
   cmd+=[str(ROOT/'tests/native/PianoLinuxFdtHost.c'),*(str(fdt/name)for name in('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')),'-o',str(exe)]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,(p.stdout+p.stderr)[:10000])
   for case in range(21):
    p=subprocess.run([str(exe),str(case)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(p.returncode,0,f'case{case}\n'+p.stdout+p.stderr)
   print('Actual cold DXE consumer:21 duplicate/truncated/CRC/recomputed-tamper/permission/bounds/copy-drift/EBS/cache-only cases PASS')
if __name__=='__main__':unittest.main()
