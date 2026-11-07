import platform
import os
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
class CpuInputTests(unittest.TestCase):
 def test_actual_gates_chunked_operations(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-cpu-input-')as directory:
   exe=Path(directory)/'input'
   subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
     '-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),
     '-I'+str(inc/HOST_PROCESSOR_BIND),'-I'+str(crypto),str(ROOT/'tests/native/PianoCpuInputTest.c'),
     str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
 def test_actual_aarch64_shared_source(self):
  inc=BASE/'MdePkg/Include'
  subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc',
    '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),
    '-I'+str(inc/'AArch64'),'-I'+str(BASE/'CryptoPkg/Include'),
    str(ROOT/'uefi/components/os-boot/PianoCpuInput.c')],check=True)
if __name__=='__main__':unittest.main()
