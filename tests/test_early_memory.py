"""Product's real SEC hook + source/ASM; no physical target execution."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
from prepare_product_early_memory import prepare,sec_source,verify
from prepare_product import fix_product_low_heap
from test_product_low_memory import function


class EarlyMemoryTests(unittest.TestCase):
    def test_actual_sec_observer_hob_path(self):
        original=(ROOT/'platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
        low,_=fix_product_low_heap(original,(ROOT/'private/captures/2026-10-03-piano/live.dtb').read_bytes())
        (ROOT/'build/product-low-memory-test.c').write_text(low)
        source=(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text()
        old=function(source,'InitializeMemory').replace('InitializeMemory','PianoActualInitializeMemory')
        new=function(sec_source(source),'InitializeMemory').replace('InitializeMemory','PianoBoundInitializeMemory')
        self.assertLess(new.index('PianoEarlyMemoryObserveCold'),new.index('LocateMemoryRegionByName'))
        self.assertLess(new.index('PrePeiSetHobList'),new.index('PianoEarlyMemoryPublishHob'))
        self.assertLess(new.index('PianoEarlyMemoryPublishHob'),new.index('Status = MemoryPeim'))
        with tempfile.TemporaryDirectory(prefix='piano-cold-sec-')as directory:
            path=Path(directory)
            (path/'PianoActualSecMemory.h').write_text(old)
            (path/'PianoActualDmaHeap.h').write_text(function((ROOT/'bootprofiles/uefi-app/PianoDma.c').read_text(),'Heap'))
            (path/'PianoBoundSecMemory.h').write_text(new)
            (path/'PianoFrozenLowFixture.h').write_text(
                (ROOT/'tests/PianoProductLowMemoryTest.c').read_text().replace('int main(void)','int PianoFrozenLowMain(void)',1))
            exe=path/'early'
            includes=[BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',
                BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',
                ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',ROOT/'tests',path]
            command=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined',
                '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',
                str(ROOT/'tests/PianoCmaPcdShim.h'),'-D_PCD_VALUE_PcdCPUCoresStackBase=0xA760D000ULL',
                '-D_PCD_VALUE_PcdCPUCorePrimaryStackSize=0x40000U']
            for include in includes:command+=['-I',str(include)]
            command +=[str(ROOT/'tests/PianoEarlyMemoryTest.c'),str(ROOT/'bootprofiles/early-memory/PianoSmemRam.c'),
                str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),
                '-Wl,--gc-sections','-o',str(exe)]
            subprocess.run(command,check=True,timeout=60)
            subprocess.run([str(exe)],check=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})

    def test_real_aarch64_read_fixup_object(self):
        source=ROOT/'bootprofiles/early-memory'
        inc=BASE/'MdePkg/Include'
        clang=ROOT/'build/host-tools/usr/bin/clang'
        with tempfile.TemporaryDirectory(prefix='piano-sec-asm-')as directory:
            obj=Path(directory)/'read.obj'
            subprocess.run([str(clang),'--target=aarch64-windows-msvc','-c',str(source/'PianoSecRead32.S'),'-o',str(obj)],check=True)
            dis=subprocess.run([str(ROOT/'build/host-tools/usr/bin/llvm-objdump'),'-d',str(obj)],check=True,capture_output=True,text=True).stdout
            self.assertRegex(dis,r'0000000000000800 <PianoSecReadVectors>')
            for instruction in ('ldr\tw9, [x0]','mrs\tx10, ESR_EL1','mrs\tx11, FAR_EL1',
                                'msr\tELR_EL1, x13','msr\tVBAR_EL1, x20','eret'):
                self.assertIn(instruction,dis)
            instructions={int(m[1],16):m[2] for line in dis.splitlines()
                if (m:=re.match(r'\s*([0-9a-f]+): [0-9a-f]{8}\s+(.+)',line))}
            # Real object slots: only current EL1 SPx sync may recover; all
            # other 15 slots, including SError/SP0/lower EL, reach fatal.
            slots=[instructions[0x800+128*i] for i in range(16)]
            targets=[re.search(r'0x([0-9a-f]+)',slot)[1]for slot in slots]
            self.assertNotEqual(targets[4],targets[0])
            self.assertEqual(len(set(targets[:4]+targets[5:])),1)
            for instruction in ('mrs\tx9, SPSel','tst\tx9, #0x1','tst\tx9, #0x4',
                'cmp\tx13, #0x25','tbz\tw10, #0x19','tbnz\tw10, #0x6',
                'tbnz\tw10, #0xa','cmp\tx13, #0x5','msr\tDAIF, x19',
                'ldr\tx14, [x2, #0x10]','smc\t#0'):
                self.assertIn(instruction,dis)
            vbar_restore=next(pc for pc,text in instructions.items()if text=='msr\tVBAR_EL1, x20')
            daif_restore=next(pc for pc,text in instructions.items()if text=='msr\tDAIF, x19')
            self.assertLess(vbar_restore,daif_restore)
            rel=subprocess.run([str(ROOT/'build/host-tools/usr/bin/llvm-objdump'),'-r',str(obj)],check=True,capture_output=True,text=True).stdout
            self.assertIn('PianoSecReadActiveState',rel)
            self.assertIn('PianoSecReadFatal',rel)
            for name in ('PianoEarlyMemory.c','PianoSmemRam.c'):
                subprocess.run([str(clang),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar',
                    '-Wall','-Wextra','-Werror','-I',str(inc),'-I',str(inc/'AArch64'),
                    '-c',str(source/name),'-o',str(Path(directory)/(name+'.obj'))],check=True)

    def test_product_staging_and_drift_rejection(self):
        with tempfile.TemporaryDirectory(prefix='piano-sec-prep-')as directory:
            target=Path(directory)/'product';target.mkdir()
            (target/'pianoProduct.dsc').write_text('[Components]\n')
            (target/'pianoProduct.fdf').write_text('[FV]\n  INF SiliciumPkg/Sec/Sec.inf\n')
            result=prepare(ROOT,target)
            self.assertTrue(verify(ROOT,target,result))
            self.assertFalse(result['high_ddr_published'])
            self.assertEqual((target/'pianoProduct.fdf').read_text().count('  INF pianoProductPkg/Sec/Sec.inf'),1)
            self.assertEqual((target/'Sec/Sec.c').read_text().count('Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);'),1)
            staged=target/'Sec/PianoSecRead32.S';raw=staged.read_bytes();staged.write_bytes(raw+b'\n// drift\n')
            with self.assertRaisesRegex(ValueError,'compiled copy stale'):verify(ROOT,target,result)
            staged.write_bytes(raw)
            bad={**result,'high_ddr_published':True}
            with self.assertRaisesRegex(ValueError,'record differs'):verify(ROOT,target,bad)
            with self.assertRaises(ValueError):prepare(ROOT,target)
        original=(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text()
        with self.assertRaises(ValueError):sec_source(original+'\n')


if __name__=='__main__':unittest.main()
