from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class NvJournalTests(unittest.TestCase):
 def test_actual_journal_fvb_powerfail_runtime(self):
  inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-nv-')as out:
   exe=Path(out)/'nv'
   subprocess.run(['cc','-O2','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Include'),str(ROOT/'tests/native/PianoNvJournalTest.c'),str(ROOT/'uefi/core/PianoNvJournal.c'),str(ROOT/'uefi/core/PianoNvFvb.c'),'-o',str(exe)],check=True)
   run=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
if __name__=='__main__':unittest.main()
