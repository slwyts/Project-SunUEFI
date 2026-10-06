from pathlib import Path
import os,re,subprocess,tempfile,unittest
import zlib
from piano_boot_objects_asm_model import run as run_shim
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
class ColdBootObjectsTests(unittest.TestCase):
 def includes(self,arch):return (BASE/'MdePkg/Include',BASE/'MdePkg/Include'/arch,BASE/'MdeModulePkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include')
 def test_actual_collector_native_map_and_hob(self):
  with tempfile.TemporaryDirectory(prefix='cold-objects-')as td:
   exe=Path(td)/'objects';cmd=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-missing-field-initializers','-Wno-unused-parameter','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-I',str(ROOT/'bootprofiles/early-memory'),'-include',str(ROOT/'tests/PianoCmaPcdShim.h'),'-D_PCD_VALUE_PcdFdBaseAddress=0xA7100000ULL','-D_PCD_VALUE_PcdFdSize=0x300000U','-D_PCD_VALUE_PcdCPUCoresStackBase=0xA760D000ULL','-D_PCD_VALUE_PcdCPUCorePrimaryStackSize=0x40000U']
   for p in self.includes('X64'):cmd+=['-I',str(p)]
   cmd+=[str(ROOT/'tests/PianoColdBootObjectsTest.c'),str(ROOT/'bootprofiles/early-memory/PianoColdBootObjectsContract.c'),str(ROOT/'bootprofiles/uefi-app/PianoProductBootObjects.c'),str(ROOT/'platforms/pianoProductPkg/Library/MemoryMapLib/MemoryMapLib.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),'-Wl,--gc-sections','-o',str(exe)]
   p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,(p.stdout+p.stderr)[:12000])
   p=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr);print(p.stdout.strip())
 def test_isolated_actual_arm64_extension_object(self):
  with tempfile.TemporaryDirectory(prefix='cold-shim-')as td:
   d=Path(td);clang=ROOT/'build/host-tools/usr/bin/clang';objdump=ROOT/'build/host-tools/usr/bin/llvm-objdump';env={**os.environ,'LD_LIBRARY_PATH':str(ROOT/'build/host-tools/usr/lib')};obj=d/'shim.o'
   subprocess.run([str(clang),'--target=aarch64-linux-gnu','-c','-DFD_BASE=0xA7100000','-DFD_SIZE=0x300000',str(ROOT/'bootprofiles/early-memory/PianoBootObjectsShim.S'),'-o',str(obj)],check=True,env=env)
   dis=subprocess.run([str(objdump),'-dr',str(obj)],check=True,capture_output=True,text=True,env=env).stdout
   table=subprocess.run([str(objdump),'-t',str(obj)],check=True,capture_output=True,text=True,env=env).stdout
   payload=int(re.search(r'(?m)^([0-9a-f]+).*\s_Payload$',table)[1],16);self.assertGreater(payload,64);self.assertEqual(payload%16,0)
   binary=d/'shim.bin';subprocess.run([str(ROOT/'build/host-tools/usr/bin/llvm-objcopy'),'-O','binary',str(obj),str(binary)],check=True,env=env);raw=binary.read_bytes();self.assertEqual(len(raw),payload);self.assertEqual(raw[56:60],b'ARM\x64');self.assertEqual(int.from_bytes(raw[8:16],'little'),0xa7100000);self.assertEqual(int.from_bytes(raw[16:24],'little'),0x300000)
   for el,in_place in ((4,False),(8,False),(4,True)):
    base=0xa7100000-len(raw) if in_place else 0xa8000000;record,mrs,steps=run_shim(raw,dis,base,el)
    self.assertEqual(int.from_bytes(record[:8],'little'),0x534e554546494448);self.assertEqual(int.from_bytes(record[8:16],'little'),0xa8500000);self.assertEqual(int.from_bytes(record[16:24],'little'),el)
    self.assertEqual(int.from_bytes(record[56:64],'little'),base);self.assertEqual(int.from_bytes(record[64:72],'little'),len(raw));self.assertEqual(int.from_bytes(record[72:80],'little'),base+len(raw));self.assertEqual(int.from_bytes(record[136:144],'little'),base+64)
    crc=int.from_bytes(record[128:132],'little');self.assertEqual(crc,zlib.crc32(record[:128]+bytes(4)+record[132:]));self.assertEqual(record[132:136],bytes(4))
    self.assertEqual(int.from_bytes(record[120:128],'little'),(63 if el==4 else 13)+(0 if in_place else 64));self.assertLess(steps,2000000)
    if el!=4:self.assertEqual(mrs,['CurrentEL']);self.assertEqual(record[40:56]+record[104:120],bytes(32))
   for code in ('str\tx9, [x7, #0x10]','str\tx10, [x7, #0x60]','str\tw10, [x7, #0x80]','mrs\tx10, VBAR_EL1','mrs\tx10, SCTLR_EL1','ldp\tx2, x3, [x4], #0x10','br\tx5'):self.assertIn(code,dis)
   # Actual branch precedes all EL1-only metadata instructions.
   self.assertLess(dis.index('b.ne'),dis.index('mrs\tx10, CNTVCT_EL0'))
   self.assertEqual((ROOT/'bootprofiles/handoff/BootShim.S').read_bytes(),(ROOT/'bootprofiles/early-memory/PianoBootObjectsShim.S').read_bytes())
   for name in ('PianoColdBootObjects.c','PianoColdBootObjectsContract.c'):
    args=[str(clang),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-D_PCD_VALUE_PcdFdBaseAddress=0xA7100000ULL','-D_PCD_VALUE_PcdFdSize=0x300000U','-D_PCD_VALUE_PcdCPUCoresStackBase=0xA760D000ULL','-D_PCD_VALUE_PcdCPUCorePrimaryStackSize=0x40000U']
    for p in self.includes('AArch64'):args+=['-I',str(p)]
    subprocess.run(args+[str(ROOT/'bootprofiles/early-memory'/name)],check=True,env=env)
if __name__=='__main__':unittest.main()
