from pathlib import Path
import os,re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
def function(text,name):
 match=re.search(r'(?m)^(?:STATIC )?EFI_STATUS\s+(?:EFIAPI\s+)?'+name+r'\s*\(',text)
 if not match:raise ValueError(name)
 start=text.index('{',match.start());end=start+1;depth=1
 while depth:
  depth += (text[end]=='{')-(text[end]=='}');end+=1
 return text[match.start():end]
class BulkIdlePipelineTests(unittest.TestCase):
 def test_actual_wait_policy_client_and_device(self):
  policy=(ROOT/'bootprofiles/uefi-app/PianoBootPolicy.c').read_text();event=(BASE/'MdeModulePkg/Core/Dxe/Event/Event.c').read_text()
  actual='\n\n'.join([function(policy,'ReadIdle'),function(policy,'Pump'),function(event,'CoreWaitForEvent')])
  inc=BASE/'MdePkg/Include';other=[BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include']
  with tempfile.TemporaryDirectory(prefix='bulk-idle-pipeline-')as directory:
   directory=Path(directory);(directory/'PianoActualBulkIdlePipeline.h').write_text(actual);exe=directory/'pipeline'
   cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-DPIANO_USB_SERVICE=1']
   for path in [inc,inc/'X64',*other,directory]:cmd+=['-I',str(path)]
   cmd += [str(ROOT/'tests/PianoBulkIdlePipelineTest.c'),str(BASE/'MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.c'),'-lcrypto','-o',str(exe)]
   result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
if __name__=='__main__':unittest.main()
