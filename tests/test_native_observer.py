"""Execute unchanged NativeProbe.c against bounded FFS/BS fixtures."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
SRC=ROOT/'bootprofiles/uefi-app/NativeProbe.c'
def header(names,capacity=False):
    data='typedef struct { CONST CHAR8 *Name; EFI_GUID Guid; CONST UINT8 *Depex; UINTN DepexBytes; } NATIVE_IMAGE;\n'
    data+='static const UINT8 True[]={6,8};\n'
    for name,guid in(('Later',0xbeef),('Never',0xf00d)):
        values=bytes([2])+struct.pack('<IHH8B',guid,0,0,*([0]*8))+bytes([8])
        data+=f'static const UINT8 {name}[]={{'+','.join(str(v)for v in values)+'};\n'
    data+='static const NATIVE_IMAGE mNativeImages[]={\n'
    for i,name in enumerate(names):
        expression='True'if capacity else'Later'if i==0 else'Never'if i==6 else'True'
        data+='{"'+name+'",{'+str(i+1)+',0,0,{0}},'+expression+',sizeof('+expression+')},\n'
    return data+'};\n'
class NativeObserverTests(unittest.TestCase):
    def test_actual_native_dispatch_order_and_current_product_capacity(self):
        product_table=ROOT/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg/Applications/ProductCore/NativeProbeTable.h'
        names=re.findall(r'\{"([^"\n]+)",\{',product_table.read_text())
        self.assertEqual(len(names),13)
        core=(ROOT/'bootprofiles/uefi-app/PianoProductCore.c').read_text()
        outer=len(re.findall(r'ObserveDisplay\("',core));self.assertEqual(outer,4)
        self.assertLessEqual(outer+2*len(names),32)
        fixtures=[(['LaterDependency','SmemDxe','MissingFv','BadLoad','BadStart','Good2','NeverReady','GpiDxe','QcomScmiDxe','UsbConfigDxe'],False),(names,True)]
        for modules,capacity in fixtures:
            with tempfile.TemporaryDirectory(prefix='native-observer-')as directory:
                directory=Path(directory);actual=directory/'NativeProbe.c';actual.write_bytes(SRC.read_bytes())
                self.assertEqual(actual.read_bytes(),SRC.read_bytes())
                (directory/'NativeProbeTable.h').write_text(header(modules,capacity))
                exe=directory/'native'
                subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-const-variable',
                    '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    *(['-DPIANO_NATIVE_FIXTURE_CAPACITY']if capacity else[]),
                    '-I'+str(INC),'-I'+str(INC/'X64'),'-I'+str(directory),str(ROOT/'tests/PianoNativeObserverTest.c'),str(actual),'-o',str(exe)],check=True)
                run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
                self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
                subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only',
                    '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-const-variable',
                    '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(directory),str(actual)],check=True)
if __name__=='__main__':unittest.main()
