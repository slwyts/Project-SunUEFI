from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class CpuInputTests(unittest.TestCase):
 def test_actual_gates_chunked_operations(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-cpu-input-')as directory:
   exe=Path(directory)/'input'
   subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
     '-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),
     '-I'+str(inc/'X64'),'-I'+str(crypto),str(ROOT/'tests/PianoCpuInputTest.c'),
     str(ROOT/'bootprofiles/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_actual_aarch64_shared_source(self):
  inc=BASE/'MdePkg/Include'
  subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
    '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),
    '-I'+str(inc/'AArch64'),'-I'+str(BASE/'CryptoPkg/Include'),
    str(ROOT/'bootprofiles/os-boot/PianoCpuInput.c')],check=True)
if __name__=='__main__':unittest.main()
