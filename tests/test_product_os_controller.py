from pathlib import Path
import os, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class ProductOsControllerTests(unittest.TestCase):
 def test_actual_preflight_owner_sfs_and_full_memory_guards(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include'
  with tempfile.TemporaryDirectory(prefix='product-os-')as directory:
   exe=Path(directory)/'controller'
   cmd=['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-missing-field-initializers','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(crypto),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(ROOT/'bootprofiles/uefi-app'),str(ROOT/'tests/PianoProductOsControllerTest.c'),str(ROOT/'bootprofiles/uefi-app/PianoProductOsController.c'),str(ROOT/'bootprofiles/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)]
   build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
   for case in range(6):
    run=subprocess.run([str(exe),str(case)],capture_output=True,text=True);self.assertEqual(run.returncode,0,f'case{case}\n'+run.stdout+run.stderr)
 def test_actual_controller_file_source_and_native_late_session(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include';fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
  with tempfile.TemporaryDirectory(prefix='product-os-joint-')as directory:
   exe=Path(directory)/'joint'
   sources=[ROOT/'tests/PianoProductOsJointTest.c',ROOT/'tests/PianoLinuxFdtHost.c',ROOT/'bootprofiles/uefi-app/PianoProductOsController.c',ROOT/'bootprofiles/os-boot/PianoBootFileSource.c',ROOT/'bootprofiles/os-boot/PianoLinuxEfiSession.c',ROOT/'bootprofiles/os-boot/PianoCpuInput.c',ROOT/'bootprofiles/uefi-app/PianoFastbootBoot.c']
   cmd=['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-DPIANO_PRODUCT_NATIVE_LATE=1','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-missing-field-initializers','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(crypto),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(ROOT/'bootprofiles/uefi-app'),'-I'+str(fdt),*map(str,sources)]
   cmd += [str(fdt/name)for name in('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')]+['-lcrypto','-o',str(exe)]
   build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
   for case in range(7):
    run=subprocess.run([str(exe),str(case)],capture_output=True,text=True);self.assertEqual(run.returncode,0,f'case{case}\n'+run.stdout+run.stderr)
 def test_actual_aarch64_syntax(self):
  inc=BASE/'MdePkg/Include';env=os.environ.copy();env['LD_LIBRARY_PATH']=str(ROOT/'build/host-tools/usr/lib')
  subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-fshort-wchar','-ffreestanding','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(inc),'-I'+str(inc/'AArch64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(ROOT/'bootprofiles/uefi-app'),str(ROOT/'bootprofiles/uefi-app/PianoProductOsController.c')],env=env,check=True)
if __name__=='__main__':unittest.main()
