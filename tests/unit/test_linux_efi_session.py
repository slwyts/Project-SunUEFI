import platform
import os
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
class LinuxEfiSessionTests(unittest.TestCase):
 def test_actual_aarch64_source_syntax(self):
  inc=BASE/'MdePkg/Include'
  subprocess.run([str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-fshort-wchar','-ffreestanding','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(inc),'-I'+str(inc/'AArch64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(ROOT/'uefi/handoff/early-memory'),str(ROOT/'uefi/components/os-boot/PianoLinuxEfiSession.c'),str(ROOT/'uefi/core/PianoPanelSelection.c'),str(ROOT/'uefi/core/PianoFastbootBoot.c')],check=True)
 def test_actual_session_libfdt_and_efi_lifecycle(self):
  inc=BASE/'MdePkg/Include';fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
  with tempfile.TemporaryDirectory(prefix='linux-efi-session-')as d:
   # Host build takes the non-AArch64 branches, EL1 DAIF access traps at EL0
   exe=Path(d)/'session';cmd=['cc','-U__aarch64__','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-missing-field-initializers','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/HOST_PROCESSOR_BIND),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(ROOT/'uefi/handoff/early-memory'),'-I'+str(fdt),str(ROOT/'tests/native/PianoLinuxEfiSessionTest.c'),str(ROOT/'tests/native/PianoLinuxFdtHost.c'),str(ROOT/'uefi/components/os-boot/PianoLinuxEfiSession.c'),str(ROOT/'uefi/core/PianoPanelSelection.c'),str(ROOT/'uefi/core/PianoFastbootBoot.c'),
     str(ROOT/'uefi/core/PianoProductBootObjects.c'),'-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
   cmd += [str(fdt/name)for name in('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')]+[
     '-Wno-deprecated-declarations','-I'+str(BASE/'CryptoPkg/Include'),str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)]
   build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
   for case in range(42):
    run=subprocess.run([str(exe),str(case)],capture_output=True,text=True);self.assertEqual(run.returncode,0,f'case{case}\n'+run.stdout+run.stderr)
if __name__=='__main__':unittest.main()
