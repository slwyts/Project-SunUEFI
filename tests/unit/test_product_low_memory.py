import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
from prepare_product import fix_product_low_heap
from plan_piano_dram import extract_dt,parse_native_c
from core_source import core_include_dirs


def function(text,name):
    m=re.search(r'STATIC\s+EFI_STATUS\s+(?:EFIAPI\s+)?'+name+r'\s*\(',text)
    if not m:raise ValueError(name)
    begin=text.index('{',m.start());depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[m.start():end]


class ProductLowMemoryTests(unittest.TestCase):
    def require_local_inputs(self, *paths):
        missing = [str(path.relative_to(ROOT)) for path in paths if not path.is_file()]
        if missing:
            self.skipTest('Optional captured/local inputs missing: ' + ', '.join(missing))

    def test_actual_product_native_table_aliases_and_current_records(self):
        import hashlib
        source=ROOT/'uefi/core/memory/PianoPlatformMemoryContract.c'
        native=ROOT/'build/firmware-workspace/uefi/platforms/pianoProductPkg/Library/MemoryMapLib/MemoryMapLib.c'
        dt=ROOT/'artifacts/linux-assembled/piano-stable-next-20261006/EFI/Piano/stable/piano.dtb'
        raw=ROOT/'private/analysis/usb-live-test95/ram402.bin'
        self.require_local_inputs(dt, raw, native)
        self.assertEqual(hashlib.sha256(dt.read_bytes()).hexdigest(),'c2cb041e2b286713c225af7bf0e3a5f7eec8926db168149984823c25ad3f38c4')
        self.assertEqual(hashlib.sha256(raw.read_bytes()).hexdigest(),'16aed6a815309af4109f34000f43de0058d310e293b2aecc29c6d4d9a7ed828c')
        libfdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        with tempfile.TemporaryDirectory(prefix='piano-actual-product-contract-')as directory:
            tmp=Path(directory);(tmp/'PianoActualProductTable.c').write_bytes(native.read_bytes());exe=tmp/'contract'
            includes=[source.parent,ROOT/'uefi/handoff/early-memory',tmp,BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',libfdt]
            cmd=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',str(ROOT/'tests/native/PianoCmaPcdShim.h')]
            for include in includes:cmd+=['-I',str(include)]
            cmd+=[str(ROOT/'tests/native/PianoPlatformProductTableTest.c'),str(ROOT/'tests/native/PianoPlatformFdtHost.c'),str(source),str(ROOT/'uefi/handoff/early-memory/PianoSmemRam.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c')]
            cmd+=[str(libfdt/name)for name in ('fdt.c','fdt_ro.c','fdt_check.c')];cmd+=['-Wl,--gc-sections','-o',str(exe)]
            build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe),str(dt),str(raw)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
        inc=BASE/'MdePkg/Include'
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(ROOT/'uefi/handoff/early-memory'),'-I'+str(inc),'-I'+str(inc/'AArch64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),str(source)],check=True)
    def test_semantic_dt_and_native_cache_preservation(self):
        self.require_local_inputs(ROOT/'private/captures/2026-10-03-piano/live.dtb')
        original=(ROOT/'uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
        dt=(ROOT/'private/captures/2026-10-03-piano/live.dtb').read_bytes()
        result,record=fix_product_low_heap(original,dt)
        new={r['name']:r for r in parse_native_c(result)}
        for row in parse_native_c(original):
            if row['name']in ('DXE_Heap','DBI_Dump'):continue
            self.assertEqual(row,new[row['name']])
        self.assertEqual(new['DXE_Heap']['base'],0xBD980000)
        self.assertEqual(new['DXE_Heap']['end'],0xD4E23000)
        self.assertFalse(record['high_ddr_added'])
        with self.assertRaises(ValueError):fix_product_low_heap(result,dt)
        newer=ROOT/'private/analysis/android-memory-2026-10-05/live.dtb'
        if newer.exists():self.assertEqual(result,fix_product_low_heap(original,newer.read_bytes())[0])

    def test_actual_sec_hob_mmu_and_dma_sources(self):
        self.require_local_inputs(ROOT/'private/captures/2026-10-03-piano/live.dtb')
        original=(ROOT/'uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
        source,_=fix_product_low_heap(original,(ROOT/'private/captures/2026-10-03-piano/live.dtb').read_bytes())
        (ROOT/'build/product-low-memory-test.c').write_text(source)
        sec=function((ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text(),'InitializeMemory').replace('InitializeMemory','PianoActualInitializeMemory')
        dma=function((ROOT/'uefi/core/memory/PianoDma.c').read_text(),'Heap')
        with tempfile.TemporaryDirectory(prefix='piano-product-low-')as tmp:
            tmp=Path(tmp);(tmp/'PianoActualSecMemory.h').write_text(sec);(tmp/'PianoActualDmaHeap.h').write_text(dma)
            exe=tmp/'low'
            includes=[ROOT/'uefi/handoff/early-memory',BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',tmp]
            command=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',str(ROOT/'tests/native/PianoCmaPcdShim.h'),'-D_PCD_VALUE_PcdCPUCoresStackBase=0xA760D000ULL','-D_PCD_VALUE_PcdCPUCorePrimaryStackSize=0x40000U']
            for include in includes:command+=['-I',str(include)]
            command+=[str(ROOT/'tests/native/PianoProductLowMemoryTest.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),'-Wl,--gc-sections','-o',str(exe)]
            build=subprocess.run(command,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())

    def test_full_ddr_actual_builder_libfdt_hob(self):
        self.require_local_inputs(ROOT/'private/captures/2026-10-03-piano/live.dtb')
        original=(ROOT/'uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text()
        source,_=fix_product_low_heap(original,(ROOT/'private/captures/2026-10-03-piano/live.dtb').read_bytes())
        (ROOT/'build/product-low-memory-test.c').write_text(source)
        libfdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        with tempfile.TemporaryDirectory(prefix='piano-full-contract-')as tmp:
            exe=Path(tmp)/'contract';includes=[ROOT/'uefi/handoff/early-memory',BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',libfdt]
            command=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',str(ROOT/'tests/native/PianoCmaPcdShim.h')]
            for include in includes:command+=['-I',str(include)]
            command+=[str(ROOT/'tests/native/PianoPlatformMemoryContractTest.c'),str(ROOT/'tests/native/PianoPlatformFdtHost.c'),str(ROOT/'uefi/core/memory/PianoPlatformMemoryContract.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c')]
            command+=[str(libfdt/name)for name in ('fdt.c','fdt_ro.c','fdt_check.c')]
            command+=['-Wl,--gc-sections','-o',str(exe)]
            build=subprocess.run(command,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe),str(ROOT/'private/captures/2026-10-03-piano/live.dtb')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())


    def test_same_boot_factory_24gb_and_live_efi_map(self):
        import struct
        from compose_piano_dtb import read_fdt, write_fdt
        data=(ROOT/'vendor/piano-linux/board.dtb').read_bytes()
        tree=read_fdt(data)
        reg=bytearray(tree['tree']['/memory']['reg'])
        expanded=False
        for offset in range(0,len(reg),16):
            base,size=struct.unpack_from('>QQ',reg,offset)
            if base==0xA00000000 and size==0x200000000:
                struct.pack_into('>QQ',reg,offset,base,0x400000000)
                expanded=True
        self.assertTrue(expanded)
        tree['tree']['/memory']['reg']=bytes(reg)
        tree['tree']['/memory']['ddr_size']=struct.pack('>I',24)
        source,_=fix_product_low_heap((ROOT/'uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c').read_text(),data)
        libfdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
        with tempfile.TemporaryDirectory(prefix='piano-cold-memory-')as directory:
            tmp=Path(directory);dt=tmp/'factory.dtb';dt.write_bytes(write_fdt(tree))
            (tmp/'PianoColdNativeTable.c').write_text(source)
            includes=[*core_include_dirs(ROOT),ROOT/'uefi/handoff/early-memory',tmp,BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',libfdt]
            command=['cc','-std=gnu11','-fshort-wchar','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-missing-field-initializers','-DPIANO_PRODUCT_MEMORY_HOST_TEST=1','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections']
            for include in includes:command+=['-I',str(include)]
            command+=[str(ROOT/'tests/native/PianoPlatformColdMemoryTest.c'),str(ROOT/'tests/native/PianoPlatformFdtHost.c'),str(ROOT/'uefi/core/memory/PianoPlatformMemoryContract.c'),str(ROOT/'uefi/core/memory/PianoProductMemory.c')]
            command+=[str(ROOT/'uefi/handoff/early-memory'/name)for name in ('PianoMemoryLayout.c','PianoColdMemory.c','PianoSmemRam.c','PianoColdBootObjectsContract.c')]
            command+=[str(libfdt/name)for name in ('fdt.c','fdt_ro.c','fdt_check.c','fdt_rw.c','fdt_wip.c')]
            command+=['-Wl,--gc-sections','-o',str(tmp/'memory')]
            build=subprocess.run(command,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(tmp/'memory'),str(dt)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
            self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())

if __name__=='__main__':unittest.main()
