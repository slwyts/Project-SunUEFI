"""Real child/Rail/GCC/Reader/Guard combination; ARM and EFI are boundaries."""
from pathlib import Path
import json,os,re,subprocess,tempfile,unittest
from test_display_clock_pipeline import function,object_comparison
from piano_mu_map_fixture import write_mu_map_fixture
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
def c_function(text,name):
 m=re.search(r'(?<![A-Za-z0-9_])(?:(?:static|STATIC)\s+)?(?:VOID|BOOLEAN|EFI_STATUS|UINT64|UINT32|UINT16|UINTN)\s+(?:EFIAPI\s+)?'+re.escape(name)+r'\s*\([^)]*\)\s*\{',text)
 if not m:raise AssertionError('missing fixture function '+name)
 p=m.end();depth=1
 while depth:
  if text[p]=='{':depth+=1
  elif text[p]=='}':depth-=1
  p+=1
 return text[m.start():p]
class NonGdscJointTests(unittest.TestCase):
 def test_real_child_rail_primary_reader_guard_and_observe(self):
  inv=json.loads((ROOT/'private/analysis/native-driver-inventory.json').read_text())['drivers']
  with tempfile.TemporaryDirectory(prefix='non-gdsc-joint-')as directory:
   d=Path(directory);write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h')
   owner=(ROOT/'uefi/core/PianoProductDisplayOwner.c').read_text();types=(ROOT/'uefi/core/PianoProductOwners.h').read_text();startup=re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} PIANO_PRODUCT_DISPLAY_STARTUP_REPORT;',types,re.S);self.assertIsNotNone(startup)
   (d/'ActualDisplayGcc.h').write_text(startup.group()+'\n'+'\n'.join(function(owner,n)for n in ('ReadAlive','LeaseAlive','ReadGcc','PianoProductDisplayOwnerRetained','CopyAcquire')))
   (d/'ActualClockObjectComparison.h').write_text(object_comparison((ROOT/'uefi/core/PianoDisplayClockRead.c').read_text()))
   fixture=(ROOT/'tests/native/PianoDisplayClockPipelineTest.c').read_text();fixture=fixture[:fixture.index('static VOID Run(UINTN Number){')]
   for source in ('uefi/components/guarded-read/PianoGuardedRead.c','uefi/core/PianoDisplayClockRead.h','uefi/components/display-rail/PianoDisplayNonGdscClock.h'):fixture=fixture.replace('"../../'+source+'"','"'+str(ROOT/source)+'"')
   fixture=fixture.replace('Events[8192]','Events[65536]')
   prototypes='''
static EFI_STATUS JointLocate(EFI_GUID *,VOID *,VOID **);
static EFI_STATUS JointHandle(EFI_HANDLE,EFI_GUID *,VOID **);
static EFI_STATUS JointSection(CONST EFI_GUID *,UINT8,UINTN,VOID **,UINTN *);
static EFI_STATUS JointMap(UINTN *,EFI_MEMORY_DESCRIPTOR *,UINTN *,UINTN *,UINT32 *);
static EFI_STATUS JointGcd(EFI_PHYSICAL_ADDRESS,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *);
static UINT32 JointLoad(UINTN);
static EFI_STATUS JointAt(UINTN,UINT64 *);
'''
   fixture=fixture.replace('#include "ActualClockObjectComparison.h"','#include "ActualClockObjectComparison.h"\n'+prototypes)
   replacements={
    'Locate':'static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **O){return JointLocate(G,R,O);}',
    'Handle':'static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **O){return JointHandle(H,G,O);}',
    'GetSectionFromAnyFv':'EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 T,UINTN I,VOID **O,UINTN *N){return JointSection(G,T,I,O,N);}',
    'Map':'static EFI_STATUS EFIAPI Map(UINTN *N,EFI_MEMORY_DESCRIPTOR *D,UINTN *K,UINTN *S,UINT32 *V){return JointMap(N,D,K,S,V);}',
    'Gcd':'static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){return JointGcd(A,D);}',
    'PianoGuardedHostLoad':'UINT32 PianoGuardedHostLoad(UINTN A){return JointLoad(A);}',
    'PianoGuardedHostAt':'EFI_STATUS PianoGuardedHostAt(UINTN A,UINT64 *P){return JointAt(A,P);}' }
   for name,body in replacements.items():fixture=fixture.replace(c_function(fixture,name),body)
   fixture=fixture.replace('static EFI_STATUS EFIAPI Close(EFI_EVENT Event){','static EFI_STATUS EFIAPI Close(EFI_EVENT Event){\n if(JointCloseWarning(Event))return EFI_WARN_STALE_DATA;')
   (d/'ActualNonGdscJointFixture.h').write_text(fixture)
   rail=(ROOT/'tests/native/PianoDisplayRailObserveTest.c').read_text()
   names=('DivU64x32Remainder','AsciiStrnLenS','StrnLenS','ReadUnaligned16','ReadUnaligned32','DebugPrintEnabled','DebugPrintLevelEnabled','DebugAssertEnabled','DebugAssert','DebugPrint','MakeGraph','StaticGraph','Relocate')
   helper='\n'.join(c_function(rail,n)for n in names).replace('static VOID Relocate(','static VOID RailRelocate(').replace('mPin[I].','JointPin[I].')
   (d/'ActualRailFixtureHelpers.h').write_text(helper)
   child=(ROOT/'tests/native/PianoDisplayNonGdscClockTest.c').read_text();native='\n'.join(c_function(child,n)for n in ('ChildBoundary','PianoNonGdscHostGet','PianoNonGdscHostChange','PianoNonGdscHostQuery'))
   native=native.replace('if(ChildCase==2)return EFI_SUCCESS;','').replace('return ChildCase==3?EFI_WARN_STALE_DATA:ChildCase==4?EFI_DEVICE_ERROR:EFI_SUCCESS;','return EFI_SUCCESS;')
   native=native.replace('if(ChildCase==16)Lost();','JointMmState();if(ChildCase==16)Lost();').replace('return ChildCase==12?EFI_WARN_STALE_DATA:ChildCase==22?EFI_DEVICE_ERROR:EFI_SUCCESS;','JointMmState();return ChildCase==12?EFI_WARN_STALE_DATA:ChildCase==22?EFI_DEVICE_ERROR:EFI_SUCCESS;')
   (d/'ActualChildNativeBoundary.h').write_text(native)
   exe=d/'joint';args=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-Wno-implicit-fallthrough','-g','-DPIANO_DISPLAY_CLOCK_HOST_TEST=1','-DPIANO_NON_GDSC_CLOCK_HOST_TEST=1','-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
   for p in (BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'uefi/components/guarded-read',ROOT/'uefi/core',ROOT/'uefi/components/display-rail',d):args+=['-I',str(p)]
   args +=[str(ROOT/'tests/native/PianoDisplayNonGdscJointTest.c'),str(ROOT/'uefi/core/PianoDisplayClockLease.c'),str(ROOT/'uefi/core/PianoDisplayClockRead.c'),str(ROOT/'uefi/core/PianoDisplayClockObserve.c'),str(ROOT/'uefi/components/display-rail/PianoDisplayNonGdscClock.c'),str(ROOT/'uefi/components/display-rail/PianoDisplayRailObserve.c'),str(BASE/'MdePkg/Library/BasePrintLib/PrintLib.c'),str(BASE/'MdePkg/Library/BasePrintLib/PrintLibInternal.c'),'-lcrypto','-o',str(exe)]
   p=subprocess.run(args,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(exe),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi'),inv['NpaDxe']['pe_path'],inv['VcsDxe']['pe_path']],capture_output=True,text=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
if __name__=='__main__':unittest.main()
