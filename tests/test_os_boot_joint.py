from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class OsBootJointTests(unittest.TestCase):
 def test_actual_file_source_linux_session_sha_libfdt(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include';fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
  sources=[ROOT/'tests/PianoOsBootJointTest.c',ROOT/'tests/PianoLinuxFdtHost.c',ROOT/'bootprofiles/os-boot/PianoBootFileSource.c',ROOT/'bootprofiles/os-boot/PianoLinuxEfiSession.c',ROOT/'bootprofiles/uefi-app/PianoFastbootBoot.c']
  with tempfile.TemporaryDirectory(prefix='osboot-joint-')as d:
   exe=Path(d)/'joint';cmd=['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(crypto),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),'-I'+str(fdt),*map(str,sources)]
   cmd += [str(fdt/name)for name in('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')]+['-lcrypto','-o',str(exe)]
   build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
   for case in range(5):
    run=subprocess.run([str(exe),str(case)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,f'case{case}\n'+run.stdout+run.stderr)
if __name__=='__main__':unittest.main()
