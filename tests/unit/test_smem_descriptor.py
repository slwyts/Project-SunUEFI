from pathlib import Path
import subprocess,tempfile,unittest,hashlib
ROOT=Path(__file__).resolve().parents[2];INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
class SmemDescriptorTests(unittest.TestCase):
 def test_actual_fixed_window_descriptor(self):
  with tempfile.TemporaryDirectory(prefix='piano-siii-')as directory:
   exe=Path(directory)/'descriptor'
   subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',
    '-I'+str(INC),'-I'+str(INC/'X64'),str(ROOT/'tests/native/PianoSmemDescriptorTest.c'),str(ROOT/'uefi/handoff/early-memory/PianoSmemDescriptor.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_exact_native_v3_layout(self):
  path=ROOT/'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.efi'
  self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),'593d9e766c0070e01d4c6684b7bb8050f32f77ea3ca300c6a110ad3903de17e3')
  dis=subprocess.run([str(ROOT/'build/host-tools/usr/bin/llvm-objdump'),'-d','--start-address=0x8b54','--stop-address=0x8cd8',str(path)],capture_output=True,text=True,check=True).stdout
  for text in ('cmp\tw8, #0x1','add\tx27, x21, #0x30','ldr\tw22, [x27, #0x14]','ldr\tw8, [x27, #0xc]',
    'ldur\tx23, [x27, #-0x8]','ldr\tx22, [x27, #0x28]','ldr\tx28, [x27]','add\tx27, x27, #0x48'):
   self.assertIn(text,dis)
if __name__=='__main__':unittest.main()
