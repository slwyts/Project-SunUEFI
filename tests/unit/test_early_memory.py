"""Product's real SEC hook + source/ASM; no physical target execution."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest
import shutil
import hashlib

ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
from prepare_product_early_memory import prepare,sec_source,verify,source_inputs
from prepare_product import fix_product_low_heap
from test_product_low_memory import function


class EarlyMemoryTests(unittest.TestCase):
    def source_fixture(self,directory):
        root=Path(directory)
        source=root/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec'
        shutil.copytree(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec',source)
        shutil.copytree(ROOT/'uefi/handoff/early-memory',root/'uefi/handoff/early-memory')
        shim=root/'uefi/handoff/bootshim/BootShim.S';shim.parent.mkdir(parents=True)
        shutil.copyfile(ROOT/'uefi/handoff/bootshim/BootShim.S',shim)
        target=root/'product';target.mkdir()
        (target/'pianoProduct.dsc').write_text('[Components]\n')
        (target/'pianoProduct.fdf').write_text('[FV]\n  INF SiliciumPkg/Sec/Sec.inf\n')
        return root,source,target

    def test_footer_variations_prepare_and_full_source_hashes_remain_bound(self):
        for newline in ('\n','\r\n'):
            with self.subTest(newline=newline),tempfile.TemporaryDirectory()as directory:
                root,source,target=self.source_fixture(directory)
                for path in source.rglob('*'):
                    if path.is_file():
                        text=path.read_bytes().decode().replace('\r\n','\n').rstrip(' \t\n')
                        footer='\n# Build annotation.\n \t\n'if path.suffix=='.inf'else '\n// Build annotation.\n/* Complete\n   independent footer. */\n \t\n'
                        path.write_bytes((text+footer).replace('\n',newline).encode())
                record=prepare(root,target);self.assertTrue(verify(root,target,record))
                for name,digest in record['source_files'].items():
                    self.assertEqual(digest,hashlib.sha256((source/name).read_bytes()).hexdigest())
                sec=source/'Sec.c';before=sec.read_bytes();sec.write_bytes(before+b'\n// Later annotation.\n')
                with self.assertRaisesRegex(ValueError,'Original SEC source drifted'):verify(root,target,record)
                sec.write_bytes(before)
                staged=target/'Sec/Sec.c';staged.write_bytes(staged.read_bytes()+b'\n// Later annotation.\n')
                with self.assertRaisesRegex(ValueError,'compiled copy stale'):verify(root,target,record)

    def test_footer_cannot_hide_sec_asm_or_inf_behavior_changes(self):
        with tempfile.TemporaryDirectory()as directory:
            _,source,_=self.source_fixture(directory)
            for name,tail in (('Sec.c','\n/* first */\nVOID Evil (VOID) {}\n/* last */\n'),
                              ('Sec.c','\n/* note */ VOID Evil (VOID) {}\n'),
                              ('Sec.c','\n/* unclosed\n'),('Sec.c','\n*/\n'),
                              ('Sec.c','\n// note \\\n'),('Sec.c','\n// note ??/\n'),
                              ('Sec.c','\r// note\n'),
                              ('AArch64/Helper.S','\n.section evil\n.byte 0\nret\n'),
                              ('Sec.inf','\n[Sources]\n  Evil.c\n'),
                              ('Sec.inf','\n[LibraryClasses]\n  EvilLib\n')):
                path=source/name;before=path.read_bytes()
                with self.subTest(name=name,tail=tail):
                    path.write_bytes(before+tail.encode())
                    with self.assertRaises(ValueError):source_inputs(source)
                path.write_bytes(before)

    def test_sec_boundary_call_count_and_order_changes_still_fail(self):
        original=(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text()
        call='  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);'
        publication='  PrePeiSetHobList (HobList);'
        for changed in (original.replace('// Locate "DXE Heap" Memory Region','// Changed memory boundary'),
                        original.replace(publication,publication+'\n'+publication),
                        original.replace(call,call+'\n'+call),
                        original.replace(call,'').replace(publication,call+'\n'+publication),
                        original.replace('#include "Sec.h"','#include "ChangedSec.h"')):
            with self.subTest(changed=changed[-100:]),self.assertRaises(ValueError):sec_source(changed)

    def test_actual_sec_observer_hob_path(self):
        original=(ROOT/'uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
        low,_=fix_product_low_heap(original,(ROOT/'private/captures/2026-10-03-piano/live.dtb').read_bytes())
        (ROOT/'build/product-low-memory-test.c').write_text(low)
        source=(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text()
        old=function(source,'InitializeMemory').replace('InitializeMemory','PianoActualInitializeMemory')
        new=function(sec_source(source),'InitializeMemory').replace('InitializeMemory','PianoBoundInitializeMemory')
        self.assertLess(new.index('PianoEarlyMemoryObserveCold'),new.index('LocateMemoryRegionByName'))
        self.assertEqual(new.count('PianoColdBootObjectsObserve ()'),1)
        self.assertEqual(new.count('PianoColdBootObjectsPublishHob ()'),1)
        self.assertLess(new.index('PianoColdBootObjectsObserve'),new.index('PianoEarlyMemoryObserveCold'))
        self.assertLess(new.index('PianoColdBootObjectsPublishHob'),new.index('Status = MemoryPeim'))
        self.assertLess(new.index('PrePeiSetHobList'),new.index('PianoEarlyMemoryPublishHob'))
        self.assertLess(new.index('PianoEarlyMemoryPublishHob'),new.index('Status = MemoryPeim'))
        with tempfile.TemporaryDirectory(prefix='piano-cold-sec-')as directory:
            path=Path(directory)
            (path/'PianoActualSecMemory.h').write_text(old)
            (path/'PianoActualDmaHeap.h').write_text(function((ROOT/'uefi/core/PianoDma.c').read_text(),'Heap'))
            (path/'PianoBoundSecMemory.h').write_text(new)
            (path/'PianoFrozenLowFixture.h').write_text(
                (ROOT/'tests/native/PianoProductLowMemoryTest.c').read_text().replace('int main(void)','int PianoFrozenLowMain(void)',1))
            exe=path/'early'
            includes=[BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',
                BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',
                ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',ROOT/'tests/native',path]
            command=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined',
                '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',
                str(ROOT/'tests/native/PianoCmaPcdShim.h'),'-D_PCD_VALUE_PcdCPUCoresStackBase=0xA760D000ULL',
                '-D_PCD_VALUE_PcdCPUCorePrimaryStackSize=0x40000U']
            for include in includes:command+=['-I',str(include)]
            command +=[str(ROOT/'tests/native/PianoEarlyMemoryTest.c'),str(ROOT/'uefi/handoff/early-memory/PianoSmemRam.c'),
                str(ROOT/'uefi/handoff/early-memory/PianoSmemDescriptor.c'),
                str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),
                '-Wl,--gc-sections','-o',str(exe)]
            subprocess.run(command,check=True,timeout=60)
            subprocess.run([str(exe)],check=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})

    def test_real_aarch64_read_fixup_object(self):
        source=ROOT/'uefi/handoff/early-memory'
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
            for name in ('PianoEarlyMemory.c','PianoSmemRam.c','PianoSmemDescriptor.c'):
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
        self.assertEqual(sec_source(original+'\n'),sec_source(original)+'\n')

    def test_cold_dxe_sources_bound_and_verified_without_sec_link(self):
        from prepare_product import core_inf
        from prepare_product_early_memory import OBJECT_DXE_FILES,OBJECT_DXE_SOURCES
        with tempfile.TemporaryDirectory(prefix='piano-cold-flat-')as directory:
            target=Path(directory);app=target/'Applications/ProductCore';app.mkdir(parents=True)
            (app/'ProductCore.inf').write_text(core_inf())
            (target/'pianoProduct.dsc').write_text('[Components]\n');(target/'pianoProduct.fdf').write_text('[FV]\n  INF SiliciumPkg/Sec/Sec.inf\n')
            record=prepare(ROOT,target);self.assertTrue(verify(ROOT,target,record))
            (app/'PianoSmemRam.h').write_bytes((ROOT/'uefi/handoff/early-memory/PianoSmemRam.h').read_bytes())
            for name in OBJECT_DXE_SOURCES:
                args=[str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
                for path in (BASE/'MdePkg/Include',BASE/'MdePkg/Include/AArch64',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',app):args+=['-I',str(path)]
                subprocess.run(args+[str(app/name)],check=True)
            sources=(app/'ProductCore.inf').read_text().split('[Sources]\n',1)[1].split('[',1)[0]
            for name in OBJECT_DXE_SOURCES:self.assertEqual(sources.splitlines().count('  '+name),1)
            self.assertNotIn('  PianoColdBootObjects.c',sources)
            for name in OBJECT_DXE_FILES:
                p=app/name;before=p.read_bytes();p.write_bytes(before+b'\n// drift\n')
                with self.assertRaisesRegex(ValueError,'copy stale'):verify(ROOT,target,record)
                p.write_bytes(before)
            inf=app/'ProductCore.inf';original=inf.read_text();inf.write_text(original.replace('  PianoProductBootObjects.c\n',''))
            with self.assertRaisesRegex(ValueError,'missing or duplicated'):verify(ROOT,target,record)


if __name__=='__main__':unittest.main()
