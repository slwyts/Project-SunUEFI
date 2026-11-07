import platform
from pathlib import Path
import importlib.util,os,re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
PUMP=ROOT/'uefi/components/product-pump/Mu_Basecore/MdePkg'
spec=importlib.util.spec_from_file_location('prepare_product_pump',ROOT/'tools/prepare_product_pump.py')
pump=importlib.util.module_from_spec(spec);spec.loader.exec_module(pump)
def function(text,name):
 match=re.search(r'(?m)^(?:STATIC )?EFI_STATUS\s+(?:EFIAPI\s+)?'+name+r'\s*\(',text)
 if not match:raise ValueError(name)
 start=text.index('{',match.start());end=start+1;depth=1
 while depth:
  depth += (text[end]=='{')-(text[end]=='}');end+=1
 return text[match.start():end]
class BulkIdlePipelineTests(unittest.TestCase):
 def test_actual_wait_policy_client_and_device(self):
  policy=(ROOT/'uefi/core/PianoBootPolicy.c').read_text();event=(BASE/'MdeModulePkg/Core/Dxe/Event/Event.c').read_text()
  # The product build installs these wait hooks into its workspace copy of Event.c
  for relative,old,new in pump.HOOKS:
   if relative==pump.BASE+'MdeModulePkg/Core/Dxe/Event/Event.c':event=pump.transform(event,old,new,relative)
  actual='\n\n'.join([function(policy,'ReadIdle'),function(policy,'Pump'),function(event,'CoreWaitForEvent')])
  inc=BASE/'MdePkg/Include';other=[BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include',PUMP/'Include',ROOT/'uefi/core']
  with tempfile.TemporaryDirectory(prefix='bulk-idle-pipeline-')as directory:
   directory=Path(directory);(directory/'PianoActualBulkIdlePipeline.h').write_text(actual);exe=directory/'pipeline'
   # Host build takes the non-AArch64 branches, EL1 DAIF access traps at EL0
   cmd=['cc','-U__aarch64__','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-DPIANO_USB_SERVICE=1']
   for path in [inc,inc/HOST_PROCESSOR_BIND,*other,directory]:cmd+=['-I',str(path)]
   cmd += [str(ROOT/'tests/native/PianoBulkIdlePipelineTest.c'),str(PUMP/'Library/PianoProductPumpLib/PianoProductPumpLib.c'),'-lcrypto','-o',str(exe)]
   result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
if __name__=='__main__':unittest.main()
