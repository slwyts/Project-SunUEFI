"""Actual protected rail object collector; no firmware or tablet execution."""
import platform
import os,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
import piano_vendor_inputs as vendor
HOST_PROCESSOR_BIND='AArch64' if platform.machine() in ('aarch64','arm64') else 'X64'
TOOLCHAIN=Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',ROOT/'build/host-tools/usr'))
INC=BASE/'MdePkg/Include';QCOM=ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'
SRC=ROOT/'uefi/components/display-rail/PianoDisplayRailObserve.c';PRINT=BASE/'MdePkg/Library/BasePrintLib'
class DisplayRailObserveTests(unittest.TestCase):
    def test_actual_guard_pin_and_typed_rail_graph(self):
        inventory=vendor.load(ROOT)['native_drivers']
        with tempfile.TemporaryDirectory(prefix='rail-observe-')as directory:
            exe=Path(directory)/'rail'
            cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
                '-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0']
            for p in (INC,INC/HOST_PROCESSOR_BIND,BASE/'CryptoPkg/Include',QCOM,ROOT/'uefi/components/guarded-read',ROOT/'uefi/core'):cmd+=['-I',str(p)]
            cmd+=[str(ROOT/'tests/native/PianoDisplayRailObserveTest.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-lcrypto','-o',str(exe)]
            result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe),str(inventory['NpaDxe']['pe_path']),str(inventory['VcsDxe']['pe_path'])],capture_output=True,text=True,
                env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
    def test_actual_aarch64_adapter(self):
        cmd=[str(TOOLCHAIN/'bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only',
            '-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
        for p in (INC,INC/'AArch64',BASE/'CryptoPkg/Include',QCOM,ROOT/'uefi/components/guarded-read',ROOT/'uefi/core'):cmd+=['-I',str(p)]
        subprocess.run(cmd+[str(SRC)],check=True)
if __name__=='__main__':unittest.main()
