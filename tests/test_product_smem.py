from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class ProductSmemTests(unittest.TestCase):
 def test_actual_three_modules_guard_parser_lifetime(self):
  inc=BASE/'MdePkg/Include';families=[ROOT/'bootprofiles/guarded-read',ROOT/'bootprofiles/early-memory'];source=ROOT/'bootprofiles/uefi-app/PianoProductSmem.c'
  with tempfile.TemporaryDirectory(prefix='product-smem-')as d:
   exe=Path(d)/'smem';flags=['-I'+str(inc),'-I'+str(inc/'X64'),*['-I'+str(p)for p in families]]
   cmd=['cc','-std=gnu11','-DPIANO_GUARDED_HOST_TEST','-DNO_MSABI_VA_FUNCS','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',*flags,str(ROOT/'tests/PianoProductSmemTest.c'),str(source),str(families[0]/'PianoGuardedRead.c'),str(families[1]/'PianoSmemRam.c'),'-o',str(exe)]
   build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
   for case in range(13):
    run=subprocess.run([str(exe),str(case)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,f'case{case}\n'+run.stdout+run.stderr)
 def test_wrapper_actual_aarch64_syntax_and_root_order(self):
  inc=BASE/'MdePkg/Include';flags=['-I'+str(inc),'-I'+str(inc/'AArch64'),'-I'+str(ROOT/'bootprofiles/guarded-read'),'-I'+str(ROOT/'bootprofiles/early-memory')]
  subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',*flags,str(ROOT/'bootprofiles/uefi-app/PianoProductSmem.c')],check=True)
  core=(ROOT/'bootprofiles/uefi-app/PianoProductCore.c').read_text();self.assertLess(core.index('PianoProbeFoundation();'),core.index('Status=PianoProductObserveSmem();'));self.assertLess(core.index('Status=PianoProductObserveSmem();'),core.index('PianoUfsSetProbeAction(InitUfs)'))
if __name__=='__main__':unittest.main()
