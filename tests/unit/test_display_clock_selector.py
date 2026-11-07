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
class ClockSelectorTests(unittest.TestCase):
 def includes(self,arch):
  return (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/arch,BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'uefi/components/guarded-read',ROOT/'uefi/core')
 def test_actual_reader_guard_and_pinned_selector(self):
  with tempfile.TemporaryDirectory(prefix='clock-selector-')as directory:
   d=Path(directory);write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h')
   fixture=(ROOT/'tests/native/PianoDisplayClockReadTest.c').read_text()
   for source in ('uefi/components/guarded-read/PianoGuardedRead.c','uefi/core/PianoDisplayClockRead.h'):
    fixture=fixture.replace('"../../'+source+'"','"'+str(ROOT/source)+'"')
   needle='UINT32 PianoGuardedHostLoad(UINTN A){';self.assertEqual(fixture.count(needle),1)
   fixture=fixture.replace(needle,needle+'\n SelectorBeforeLoad(A);')
   (d/'ActualClockSelectorFixture.h').write_text(fixture)
   cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie']
   for p in self.includes(HOST_PROCESSOR_BIND):cmd+=['-I',str(p)]
   cmd+=['-I',directory,str(ROOT/'tests/native/PianoDisplayClockSelectorTest.c'),str(ROOT/'uefi/core/PianoDisplayClockRead.c'),'-Wl,--wrap=PianoGuardedReadBegin','-Wl,--wrap=PianoGuardedReadEnd','-lcrypto','-o',str(d/'selector')]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   # Same ClockDxe bytes that prepare_product stages from the vendor bundle
   clock=Path(directory)/'ClockDxe.efi';clock.write_bytes(derive(vendor.load(ROOT)['native_drivers']['ClockDxe']['pe_path'].read_bytes())[0])
   p=subprocess.run([str(d/'selector'),str(clock)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_actual_aarch64_reader_and_combined_observer_headers(self):
  with tempfile.TemporaryDirectory(prefix='clock-selector-a64-')as directory:
   p=Path(directory)/'combined.c';p.write_text('#include "PianoDisplayClockRead.h"\n#include "PianoDisplayClockObserve.h"\n')
   env=os.environ.copy();env['LD_LIBRARY_PATH']=str(TOOLCHAIN/'lib')
   args=[str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
   for q in self.includes('AArch64'):args+=['-I',str(q)]
   subprocess.run(args+[str(ROOT/'uefi/core/PianoDisplayClockRead.c'),str(p)],env=env,check=True)
if __name__=='__main__':unittest.main()
