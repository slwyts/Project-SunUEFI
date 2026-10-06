"""Exact display mapping candidate source, drift/refusal and actual Mu tests."""
import copy
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
import piano_display_mapping as mapping


def actual_function(text,name):
    match=re.search(r'(?m)^(?:STATIC\s+)?(?:UINT64|UINTN|BOOLEAN)\s+(?:EFIAPI\s+)?'+name+r'\s*\(',text)
    if not match:raise ValueError('Missing actual function '+name)
    at=match.start();begin=text.index('{',at);depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[at:end]


class DisplayMappingTests(unittest.TestCase):
    def fixture(self,directory):
        root=Path(directory)
        for path in mapping.source_files(ROOT)+(ROOT/mapping.NATIVE,):
            target=root/path.relative_to(ROOT);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        return root

    def test_pinned_prepare_verify_and_small_fixture_drift(self):
        native=mapping.original_native(ROOT);text,record=mapping.prepare(ROOT,native)
        self.assertTrue(mapping.verify(ROOT,text,record));self.assertEqual(record['candidate_rows'],52)
        self.assertEqual([r['size'] for r in record['windows']],[0x1f5000,0x94000,0x20000])
        self.assertFalse(record['hardware_verified']);self.assertFalse(record['register_access_authorized'])
        with tempfile.TemporaryDirectory(prefix='display-pin-')as directory:
            root=self.fixture(directory);self.assertEqual(mapping.prepare(root,native),(text,record))
            (root/mapping.NATIVE).write_text(text)
            self.assertEqual(mapping.original_native(root),native)
            self.assertEqual(mapping.create(root),(text.encode(),record))
            for path in mapping.source_files(root):
                original=path.read_bytes();path.write_bytes(original+b'\n')
                with self.assertRaises(ValueError):mapping.verify(root,text,record)
                path.write_bytes(original)
        for changed in (text.replace('0x1F5000','0x1F4000'),text.replace('EfiMemoryMappedIO','EfiConventionalMemory'),text+'\n',text+mapping.MARKER):
            with self.assertRaises(ValueError):mapping.verify(ROOT,changed,record)
        changed=copy.deepcopy(record);changed['hardware_verified']=True
        with self.assertRaises(ValueError):mapping.verify(ROOT,text,changed)
        with self.assertRaises(ValueError):mapping.prepare(ROOT,native+'\n')

    def test_semantic_owner_bounds_and_raw_reg_refusals(self):
        inputs={p:(ROOT/p).read_bytes()for p in mapping.PINS if p!=mapping.NATIVE};native=mapping.original_native(ROOT).encode()
        for name,path,base,raw_size,page_size in mapping.WINDOWS:
            with self.assertRaisesRegex(ValueError,'conflicts'):mapping.validate(inputs,native,[{'name':'owned-page','base':base+page_size-1,'size':1}])
        for owner in ({'name':'overflow','base':(1<<64)-1,'size':1},{'name':'zero','base':0,'size':0},{'name':'extra','base':0,'size':1,'ready':True}):
            with self.assertRaises(ValueError):mapping.validate(inputs,native,[owner])
        with self.assertRaisesRegex(ValueError,'conflicts'):mapping.validate(inputs,native.replace(b'0xB000000, 0x4000000',b'0xAF00000, 0x20000'))
        tree_raw=inputs[mapping.DTB];changed=dict(inputs)
        old=bytes.fromhex('00100000001f4200');new=bytes.fromhex('00100000001f4000');self.assertIn(old,tree_raw)
        changed[mapping.DTB]=tree_raw.replace(old,new,1)
        with self.assertRaisesRegex(ValueError,'exact reg'):mapping.validate(changed,native)
        changed=dict(inputs);changed[mapping.DTS]=inputs[mapping.DTS].replace(b'0x1f4200',b'0x1f4000')
        with self.assertRaisesRegex(ValueError,'Kernel resource'):mapping.validate(changed,native)

    def test_actual_mu_hobs_typed_rows_and_arm_mmu_device(self):
        native=mapping.original_native(ROOT);expanded,record=mapping.prepare(ROOT,native)
        mmu=(ROOT/'upstream/Mu-Silicium/Mu_Basecore/UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c').read_text()
        cpu=(ROOT/'upstream/Mu-Silicium/Mu_Basecore/ArmPkg/Drivers/CpuDxe/AArch64/Mmu.c').read_text()
        gcd=(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/Gcd/Gcd.c').read_text()
        dxe_header=(ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/DxeMain.h').read_text()
        with tempfile.TemporaryDirectory(prefix='display-real-mu-')as directory:
            temp=Path(directory);(temp/'PianoDisplayOriginal.c').write_text(native);(temp/'PianoDisplayCandidate.c').write_text(expanded)
            (temp/'PianoDisplayActualArmMmu.h').write_text(actual_function(mmu,'TranslationRegimeIsDual')+'\n'+actual_function(mmu,'ArmMemoryAttributeToPageAttribute'))
            conversion=re.search(r'GCD_ATTRIBUTE_CONVERSION_ENTRY\s+mAttributeConversionTable\[\]\s*=\s*\{.*?\n\};',gcd,re.S)[0]
            branch=re.search(r'case EFI_RESOURCE_MEMORY_MAPPED_IO:\s*case EFI_RESOURCE_FIRMWARE_DEVICE:\s*GcdMemoryType = EfiGcdMemoryTypeMemoryMappedIo;\s*break;',gcd)[0]
            definitions='\n'.join(re.findall(r'(?m)^#define EFI_MEMORY_(?:PRESENT|INITIALIZED|TESTED)\s+[^\n]+',dxe_header))
            header=definitions+'\n'+'typedef struct {UINT64 Attribute,Capability;BOOLEAN Memory;} GCD_ATTRIBUTE_CONVERSION_ENTRY;\n'+conversion+'\n'
            header+=actual_function(gcd,'CoreConvertResourceDescriptorHobAttributesToCapabilities')+'\n'+actual_function(cpu,'PageAttributeToGcdAttribute')+'\n'
            header+='STATIC EFI_GCD_MEMORY_TYPE ActualHobMmioType(EFI_RESOURCE_TYPE Type){EFI_GCD_MEMORY_TYPE GcdMemoryType=EfiGcdMemoryTypeNonExistent;switch(Type){'+branch.replace('ResourceHob->ResourceType','Type')+'default:break;}return GcdMemoryType;}\n'
            (temp/'PianoDisplayActualGcd.h').write_text(header)
            exe=temp/'mapping';includes=[temp,BASE/'MdePkg/Include',BASE/'MdePkg/Include/X64',BASE/'MdeModulePkg/Include',BASE/'UefiCpuPkg/Include',BASE/'EmbeddedPkg/Include',ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include']
            cmd=['cc','-std=gnu11','-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-include',str(ROOT/'tests/PianoCmaPcdShim.h')]
            for include in includes:cmd+=['-I',str(include)]
            cmd+=[str(ROOT/'tests/PianoDisplayMappingContractTest.c'),str(BASE/'EmbeddedPkg/Library/PrePiHobLib/Hob.c'),'-Wl,--gc-sections','-o',str(exe)]
            build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
            inc=BASE/'MdePkg/Include'
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),'-I'+str(inc/'AArch64'),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include'),str(temp/'PianoDisplayCandidate.c')],check=True)


if __name__=='__main__':unittest.main()
