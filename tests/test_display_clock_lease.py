from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class DisplayClockLeaseTests(unittest.TestCase):
 def test_actual_pinned_native_identity_refs_and_guard_boundaries(self):
  inc=BASE/'MdePkg/Include';crypto=BASE/'CryptoPkg/Include';qcom=ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'
  with tempfile.TemporaryDirectory(prefix='display-clock-lease-')as directory:
   exe=Path(directory)/'lease';cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-DPIANO_DISPLAY_CLOCK_HOST_TEST=1','-fsanitize=address,undefined','-fno-pie','-no-pie']
   for path in (inc,inc/'X64',crypto,qcom):cmd+=['-I',str(path)]
   cmd += [str(ROOT/'tests/PianoDisplayClockLeaseTest.c'),str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockLease.c'),'-lcrypto','-o',str(exe)]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   p=subprocess.run([str(exe),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_actual_aarch64_source(self):
  inc=BASE/'MdePkg/Include';env=os.environ.copy();env['LD_LIBRARY_PATH']=str(ROOT/'build/host-tools/usr/lib')
  args=[str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
  for path in (inc,inc/'AArch64',BASE/'CryptoPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'):args+=['-I',str(path)]
  subprocess.run(args+[str(ROOT/'bootprofiles/uefi-app/PianoDisplayClockLease.c')],env=env,check=True)
if __name__=='__main__':unittest.main()
