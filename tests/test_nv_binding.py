from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class NvBindingTests(unittest.TestCase):
 def test_runtime_code_memory_and_no_late_override(self):
  inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='nv-binding-')as d:
   exe=Path(d)/'bind'
   subprocess.run(['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include'),str(ROOT/'tests/PianoNvBindingTest.c'),str(ROOT/'bootprofiles/uefi-app/PianoNvFvbBinding.c'),'-o',str(exe)],check=True)
   for case in range(4):subprocess.run([str(exe),str(case)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()
