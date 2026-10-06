"""Actual protected rail object collector; no firmware or tablet execution."""
import json,os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
INC=BASE/'MdePkg/Include';QCOM=ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'
SRC=ROOT/'bootprofiles/display-rail/PianoDisplayRailObserve.c';PRINT=BASE/'MdePkg/Library/BasePrintLib'
class DisplayRailObserveTests(unittest.TestCase):
    def test_actual_guard_pin_and_typed_rail_graph(self):
        inventory=json.loads((ROOT/'private/analysis/native-driver-inventory.json').read_text())['drivers']
        with tempfile.TemporaryDirectory(prefix='rail-observe-')as directory:
            exe=Path(directory)/'rail'
            cmd=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
                '-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0']
            for p in (INC,INC/'X64',BASE/'CryptoPkg/Include',QCOM,ROOT/'bootprofiles/guarded-read',ROOT/'bootprofiles/uefi-app'):cmd+=['-I',str(p)]
            cmd+=[str(ROOT/'tests/PianoDisplayRailObserveTest.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-lcrypto','-o',str(exe)]
            result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe),inventory['NpaDxe']['pe_path'],inventory['VcsDxe']['pe_path']],capture_output=True,text=True,
                env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
    def test_actual_aarch64_adapter(self):
        cmd=[str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only',
            '-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
        for p in (INC,INC/'AArch64',BASE/'CryptoPkg/Include',QCOM,ROOT/'bootprofiles/guarded-read',ROOT/'bootprofiles/uefi-app'):cmd+=['-I',str(p)]
        subprocess.run(cmd+[str(SRC)],check=True)
if __name__=='__main__':unittest.main()
