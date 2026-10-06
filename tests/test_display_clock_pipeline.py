"""Actual combined clock pipeline; only machine/EFI/native ARM boundaries mocked."""
from pathlib import Path
import hashlib,os,re,subprocess,tempfile,unittest
from piano_mu_map_fixture import write_mu_map_fixture
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
def function(text,name):
 match=re.search(r'(?:STATIC\s+)?(?:VOID|BOOLEAN|EFI_STATUS)\s+'+re.escape(name)+r'\s*\([^)]*\)\s*\{',text)
 if not match:raise AssertionError('missing actual Root function '+name)
 pos=match.end();depth=1
 while depth:
  if text[pos]=='{':depth+=1
  elif text[pos]=='}':depth-=1
  pos+=1
 return text[match.start():pos]
def object_comparison(text):
 # Compile the actual private type/helper verbatim to poison representation
 # padding explicitly, without copying the comparison implementation.
 match=re.search(r'typedef struct \{PIANO_DISPLAY_CLOCK_READ_ROLE Role;UINT64 Base,Bytes,Anchor;\} DCR_OBJECT;',text)
 if not match:raise AssertionError('missing actual bounded-object type')
 return match.group()+'\n'+function(text,'DcrSameObject')+'\n'
class DisplayClockPipelineTests(unittest.TestCase):
 def test_actual_combined_native_clock_reader_and_guard_pipeline(self):
  owner=ROOT/'bootprofiles/uefi-app/PianoProductDisplayOwner.c';text=owner.read_text()
  with tempfile.TemporaryDirectory(prefix='display-clock-pipeline-')as directory:
   d=Path(directory);owners=(ROOT/'bootprofiles/uefi-app/PianoProductOwners.h').read_text();startup=re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} PIANO_PRODUCT_DISPLAY_STARTUP_REPORT;',owners,re.S)
   if not startup:raise AssertionError('missing actual Root startup report')
   (d/'ActualDisplayGcc.h').write_text('// Verbatim actual Root adapter source SHA256 '+hashlib.sha256(owner.read_bytes()).hexdigest()+'\n'+startup.group()+'\n'+'\n'.join(function(text,n)for n in ('ReadAlive','LeaseAlive','ReadGcc','PianoProductDisplayOwnerRetained','CopyAcquire'))+'\n')
   (d/'ActualClockObjectComparison.h').write_text(object_comparison((ROOT/'bootprofiles/uefi-app/PianoDisplayClockRead.c').read_text()))
   write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h')
   # Actual Lease directly dereferences the native protocol and actual Reader
   # enforces the original low heap. ASAN reserves that x64 address-space gap;
   # use UBSAN here without weakening either source's real address contract.
   # Separate actual Lease/Reader/Guard tests retain their ASAN coverage.
   exe=d/'pipeline';cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-DPIANO_DISPLAY_CLOCK_HOST_TEST=1','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
   for p in (BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',ROOT/'bootprofiles/guarded-read',d):cmd+=['-I',str(p)]
   cmd+=[str(ROOT/'tests/PianoDisplayClockPipelineTest.c'),str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockRead.c'),str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockLease.c'),'-lcrypto','-o',str(exe)]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(exe),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi')],capture_output=True,text=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
if __name__=='__main__':unittest.main()
