import platform
from pathlib import Path
import os,subprocess,sys,tempfile,unittest
from piano_mu_map_fixture import write_mu_map_fixture
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
import piano_vendor_inputs as vendor
from piano_inherited_clock import derive
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
class DisplayClockReadTests(unittest.TestCase):
 def includes(self,arch):
  return (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/arch,BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'uefi/components/guarded-read')
 def test_actual_reader_with_actual_guard(self):
  with tempfile.TemporaryDirectory(prefix='display-clock-read-')as directory:
   write_mu_map_fixture(ROOT,Path(directory)/'ActualMuMemoryMap.h')
   exe=Path(directory)/'read';cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie']
   for path in self.includes(HOST_PROCESSOR_BIND):cmd+=['-I',str(path)]
   cmd+=['-I',directory]
   cmd+=[str(ROOT/'tests/native/PianoDisplayClockReadTest.c'),str(ROOT/'uefi/core/PianoDisplayClockRead.c'),'-Wl,--wrap=PianoGuardedReadBegin','-Wl,--wrap=PianoGuardedReadEnd','-lcrypto','-o',str(exe)]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   # Same ClockDxe bytes that prepare_product stages from the vendor bundle
   clock=Path(directory)/'ClockDxe.efi';clock.write_bytes(derive(vendor.load(ROOT)['native_drivers']['ClockDxe']['pe_path'].read_bytes())[0])
   p=subprocess.run([str(exe),str(clock)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_actual_aarch64_flat_staging_source(self):
  with tempfile.TemporaryDirectory(prefix='display-clock-flat-')as directory:
   d=Path(directory)
   for p in ('PianoDisplayClockRead.c','PianoDisplayClockRead.h','PianoDisplayClockLease.h'):(d/p).write_bytes((ROOT/'uefi/core'/p).read_bytes())
   (d/'PianoGuardedRead.h').write_bytes((ROOT/'uefi/components/guarded-read/PianoGuardedRead.h').read_bytes())
   env=os.environ.copy();env['LD_LIBRARY_PATH']=str(TOOLCHAIN/'lib');args=[str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
   for p in self.includes('AArch64')[:-1]:args+=['-I',str(p)]
   subprocess.run(args+[str(d/'PianoDisplayClockRead.c')],env=env,check=True)
if __name__=='__main__':unittest.main()
