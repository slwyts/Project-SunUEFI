from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class LargeDownloadTests(unittest.TestCase):
 def test_actual_source_large_owner_hash_copy_zero(self):
  inc=BASE/'MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-large-blob-')as directory:
   exe=Path(directory)/'blob'
   subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
     '-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined',
     '-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(BASE/'CryptoPkg/Include'),
     '-I'+str(ROOT/'bootprofiles/uefi-app'),str(ROOT/'tests/PianoLargeDownloadBlobTest.c'),
     str(ROOT/'bootprofiles/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=60)
if __name__=='__main__':unittest.main()
