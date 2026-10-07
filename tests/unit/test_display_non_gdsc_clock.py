from pathlib import Path
import os,re,subprocess,tempfile,unittest
from test_display_clock_pipeline import function,object_comparison
from piano_mu_map_fixture import write_mu_map_fixture
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class NonGdscClockTests(unittest.TestCase):
 def includes(self,arch):
  return (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/arch,BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'uefi/components/guarded-read',ROOT/'uefi/core',ROOT/'uefi/components/display-rail')
 def test_actual_owner_primary_lease_reader_and_guard(self):
  with tempfile.TemporaryDirectory(prefix='non-gdsc-owner-')as directory:
   d=Path(directory);write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h');owner=(ROOT/'uefi/core/PianoProductDisplayOwner.c').read_text();types=(ROOT/'uefi/core/PianoProductOwners.h').read_text();startup=re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} PIANO_PRODUCT_DISPLAY_STARTUP_REPORT;',types,re.S);self.assertIsNotNone(startup)
   (d/'ActualDisplayGcc.h').write_text(startup.group()+'\n'+'\n'.join(function(owner,n)for n in ('ReadAlive','LeaseAlive','ReadGcc','PianoProductDisplayOwnerRetained','CopyAcquire')))
   (d/'ActualClockObjectComparison.h').write_text(object_comparison((ROOT/'uefi/core/PianoDisplayClockRead.c').read_text()))
   fixture=(ROOT/'tests/native/PianoDisplayClockPipelineTest.c').read_text();fixture=fixture[:fixture.index('static VOID Run(UINTN Number){')]
   for source in ('uefi/components/guarded-read/PianoGuardedRead.c','uefi/core/PianoDisplayClockRead.h','uefi/components/display-rail/PianoDisplayNonGdscClock.h'):fixture=fixture.replace('"../../'+source+'"','"'+str(ROOT/source)+'"')
   fixture=fixture.replace('Events[8192]','Events[32768]');needle='static EFI_STATUS EFIAPI Close(EFI_EVENT Event){';self.assertEqual(fixture.count(needle),1);fixture=fixture.replace(needle,needle+'\n if(ChildCloseWarning(Event))return EFI_WARN_STALE_DATA;')
   (d/'ActualNonGdscFixture.h').write_text(fixture)
   cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-g','-DPIANO_DISPLAY_CLOCK_HOST_TEST=1','-DPIANO_NON_GDSC_CLOCK_HOST_TEST=1','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
   for p in (*self.includes('X64'),d):cmd+=['-I',str(p)]
   cmd +=[str(ROOT/'tests/native/PianoDisplayNonGdscClockTest.c'),str(ROOT/'uefi/core/PianoDisplayClockRead.c'),str(ROOT/'uefi/core/PianoDisplayClockLease.c'),str(ROOT/'uefi/components/display-rail/PianoDisplayNonGdscClock.c'),'-lcrypto','-o',str(d/'owner')]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(d/'owner'),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi')],capture_output=True,text=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_actual_aarch64_owner(self):
  env=os.environ.copy();env['LD_LIBRARY_PATH']=str(ROOT/'build/host-tools/usr/lib');args=[str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
  for p in self.includes('AArch64'):args+=['-I',str(p)]
  subprocess.run(args+[str(ROOT/'uefi/components/display-rail/PianoDisplayNonGdscClock.c')],env=env,check=True)
if __name__=='__main__':unittest.main()
