from pathlib import Path
import os,subprocess,tempfile,unittest
from piano_mu_map_fixture import write_mu_map_fixture
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class ClockSelectorTests(unittest.TestCase):
 def includes(self,arch):
  return (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/arch,BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'bootprofiles/guarded-read',ROOT/'bootprofiles/uefi-app')
 def test_actual_reader_guard_and_pinned_selector(self):
  with tempfile.TemporaryDirectory(prefix='clock-selector-')as directory:
   d=Path(directory);write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h')
   fixture=(ROOT/'tests/PianoDisplayClockReadTest.c').read_text()
   for source in ('guarded-read/PianoGuardedRead.c','uefi-app/PianoDisplayClockRead.h'):
    fixture=fixture.replace('"../bootprofiles/'+source+'"','"'+str(ROOT/'bootprofiles'/source)+'"')
   needle='UINT32 PianoGuardedHostLoad(UINTN A){';self.assertEqual(fixture.count(needle),1)
   fixture=fixture.replace(needle,needle+'\n SelectorBeforeLoad(A);')
   (d/'ActualClockSelectorFixture.h').write_text(fixture)
   cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie']
   for p in self.includes('X64'):cmd+=['-I',str(p)]
   cmd+=['-I',directory,str(ROOT/'tests/PianoDisplayClockSelectorTest.c'),str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockRead.c'),'-Wl,--wrap=PianoGuardedReadBegin','-Wl,--wrap=PianoGuardedReadEnd','-lcrypto','-o',str(d/'selector')]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(d/'selector'),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_actual_aarch64_reader_and_combined_observer_headers(self):
  with tempfile.TemporaryDirectory(prefix='clock-selector-a64-')as directory:
   p=Path(directory)/'combined.c';p.write_text('#include "PianoDisplayClockRead.h"\n#include "PianoDisplayClockObserve.h"\n')
   env=os.environ.copy();env['LD_LIBRARY_PATH']=str(ROOT/'build/host-tools/usr/lib')
   args=[str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
   for q in self.includes('AArch64'):args+=['-I',str(q)]
   subprocess.run(args+[str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockRead.c'),str(p)],env=env,check=True)
if __name__=='__main__':unittest.main()
