"""Actual product staging/INF and freshness for the shared OS loader, no device."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import prepare_product as product
import build_integrity as integrity
from prepare_product_early_memory import prepare as prepare_early, OBJECT_DXE_FILES
from prepare_product_handoff import prepare as prepare_handoff, stage_provider


class ProductOsBootWiringTests(unittest.TestCase):
    def fixture(self,root):
        import piano_vendor_inputs as vendor
        shutil.copytree(ROOT/'vendor/piano',root/'vendor/piano')
        for relative in ('tools/piano_vendor_inputs.py','tools/apply_firmware_patches.py'):
            destination=root/relative;destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/relative,destination)
        shutil.copytree(ROOT/'patches/firmware',root/'patches/firmware')
        shutil.copytree(ROOT/'uefi/components/os-boot',root/'uefi/components/os-boot')
        for family in product.OBSERVATION_FAMILIES:shutil.copytree(product.observation_directory(ROOT,family),product.observation_directory(root,family))
        shared=root/'uefi/core';shared.mkdir(parents=True)
        for name,path in product.shared_boot_headers(ROOT).items():
            target=shared/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        for name in product.SOURCE_NAMES:(shared/name).write_bytes((ROOT/'uefi/core'/name).read_bytes())
        for relative in OBJECT_DXE_FILES.values():
            target=root/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/relative,target)
        shutil.copytree(ROOT/'uefi/handoff/bootshim',root/'uefi/handoff/bootshim')
        app=root/'uefi/platforms/pianoProductPkg/Applications/ProductCore';app.mkdir(parents=True)
        for name in product.SOURCE_NAMES:shutil.copyfile(shared/name,app/name)
        for name,path in product.shared_boot_headers(root).items():
            target=app/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        record=product.prepare_os_boot(root,app)
        shutil.copytree(ROOT/'uefi/components/product-handoff',root/'uefi/components/product-handoff')
        for relative in prepare_handoff(ROOT,apply=False)['files']:
            src=ROOT/relative;dst=root/relative;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
        native=prepare_handoff(root,apply=True)
        provider=stage_provider(root,app)
        observation=product.prepare_observation_families(root,app)
        (app/'ProductCore.inf').write_text(product.core_inf())
        sec=Path('upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec')
        shutil.copytree(ROOT/sec,root/sec)
        target=app.parent.parent
        (target/'pianoProduct.dsc').write_text('[Components]\n')
        (target/'pianoProduct.fdf').write_text('[FV]\n  INF SiliciumPkg/Sec/Sec.inf\n')
        early=prepare_early(root,target)
        import piano_display_mapping as display
        for path in display.source_files(ROOT):
            copy=root/path.relative_to(ROOT);copy.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,copy)
        for name in ('tools/compose_piano_dtb.py','tools/analyze_capture.py','tools/source_input_tail.py'):
            copy=root/name;copy.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,copy)
        memory,display_candidate=display.prepare(root,display.original_native(ROOT))
        memory_path=target/'Library/MemoryMapLib/MemoryMapLib.c';memory_path.parent.mkdir(parents=True,exist_ok=True);memory_path.write_text(memory)
        display_binding={'candidate':display_candidate,'compiled_into_product':True,'hardware_verified':False,'register_access_authorized':False}
        staged=root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg'
        shutil.copytree(app.parent.parent,staged)
        manifest=root/'build/product/prepared-manifest.json';manifest.parent.mkdir(parents=True);manifest.write_text(json.dumps({'vendor_inputs':vendor.record(root),'os_boot':record,'dxe_observation':observation,'early_memory':early,'native_late_handoff':native,'late_provider':provider,'display_mapping':display_binding}))
        for relative in ('config/piano-product.json','tools/prepare_product.py','tools/build_product.sh','tools/package_product.py',
                         'tools/prepare_product_pump.py','tools/prepare_product_ui.py','tools/prepare_nv_runtime_guard.py','tools/prepare_product_early_memory.py','tools/prepare_product_handoff.py','tools/simpleinit_build_identity.py',
                         'tools/build_simpleinit.sh','tools/prepare_simpleinit.py','tools/product_payload_digest.py',
                         'artifacts/simpleinit/product/SimpleInit.efi','artifacts/simpleinit/product/app-payload.bin','artifacts/simpleinit/product/build-ok.json'):
            path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture')
        return app,record,staged/'Applications/ProductCore'

    def refresh_early(self,root,app):
        target=app.parent.parent
        shutil.rmtree(target/'Sec')
        (target/'pianoProduct.dsc').write_text('[Components]\n')
        (target/'pianoProduct.fdf').write_text('[FV]\n  INF SiliciumPkg/Sec/Sec.inf\n')
        # Recreate the actual base INF, retaining its OS/family wiring. The
        # early helper must still reject duplicate cold-source binding.
        (app/'ProductCore.inf').write_text(product.core_inf())
        record=prepare_early(root,target)
        staged=root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg'
        shutil.rmtree(staged/'Sec');shutil.copytree(target/'Sec',staged/'Sec')
        for name in ('pianoProduct.dsc','pianoProduct.fdf'):shutil.copyfile(target/name,staged/name)
        for name in (*OBJECT_DXE_FILES,'ProductCore.inf'):shutil.copyfile(app/name,staged/'Applications/ProductCore'/name)
        path=root/'build/product/prepared-manifest.json';manifest=json.loads(path.read_text());manifest['early_memory']=record;path.write_text(json.dumps(manifest))

    def fingerprint(self,root):
        with patch('prepare_product_pump.prepare',return_value={'files':{}}),patch('prepare_product_ui.prepare',return_value={'files':{}}),patch('prepare_nv_runtime_guard.prepare',return_value={'files':{}}):
            return integrity.inputs(root,'product')

    def test_actual_staged_sources_compile_from_inf_without_rewriting(self):
        with tempfile.TemporaryDirectory(prefix='product-os-wiring-')as tmp:
            root=Path(tmp);app,record,staged=self.fixture(root)
            self.assertTrue(product.verify_os_boot(root,app,record));self.assertTrue(product.verify_os_boot(root,staged,record))
            self.assertFalse(record['platform_bound']);self.assertFalse(record['full_ddr_verified']);self.assertFalse(record['start_enabled'])
            inf=(staged/'ProductCore.inf').read_text();section=inf.split('[Sources]\n',1)[1].split('[Packages]',1)[0]
            bound=[line.strip()for line in section.splitlines()if line.strip().startswith('Components/os-boot/')]
            self.assertEqual(bound,list(product.OS_BOOT_INF_SOURCES))
            for name,path in product.os_boot_files(root).items():self.assertEqual((staged/'Components/os-boot'/name).read_bytes(),path.read_bytes())
            for guid in ('gFdtTableGuid','gEfiEventBeforeExitBootServicesGuid','gLinuxEfiInitrdMediaGuid','gEfiLoadFile2ProtocolGuid'):
                self.assertIn(guid,inf)
            include=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include';crypto=ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include'
            for name in bound:
                if name.endswith('.c'):
                    output=root/(Path(name).stem+'.obj')
                    subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-c','-Wall','-Wextra','-Werror',
                                    '-I',str(include),'-I',str(include/'AArch64'),'-I',str(crypto),'-I',str(staged),str(staged/name),'-o',str(output)],check=True)
                    self.assertGreater(output.stat().st_size,0)

    def test_freshness_rejects_canonical_staged_header_and_inf_mutations(self):
        mutations=('canonical_c','canonical_h','canonical_missing','staged_c','staged_h','relative_header','module_header','inf_source','inf_library')
        for mutation in mutations:
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory(prefix='product-os-fresh-')as tmp:
                root=Path(tmp);app,record,staged=self.fixture(root);before=self.fingerprint(root)
                if mutation.startswith('canonical_'):
                    name='PianoBootFileSource.h'if mutation=='canonical_h'else 'PianoBootFileSource.c';path=root/'uefi/components/os-boot'/name
                    if mutation=='canonical_missing':path.unlink()
                    else:path.write_text(path.read_text()+'\n// changed\n')
                elif mutation in ('staged_c','staged_h'):
                    path=staged/'Components/os-boot'/('PianoLinuxEfiSession.c'if mutation=='staged_c'else 'PianoLinuxEfiSession.h');path.write_text(path.read_text()+'\n// changed\n')
                elif mutation in ('relative_header','module_header'):
                    path=staged/('core/PianoFastbootLaunch.h'if mutation=='relative_header'else 'PianoFastbootLaunch.h');path.write_text(path.read_text()+'\n// changed\n')
                else:
                    path=staged/'ProductCore.inf';path.write_text(path.read_text().replace('  Components/os-boot/PianoCpuImageLoan.c\n'if mutation=='inf_source'else '  SynchronizationLib\n',''))
                with self.assertRaises(ValueError):self.fingerprint(root)
                self.assertGreater(before['file_count'],6)

    def test_coherent_source_reprepare_changes_build_input_fingerprint(self):
        with tempfile.TemporaryDirectory(prefix='product-os-reprepare-')as tmp:
            root=Path(tmp);app,record,staged=self.fixture(root);before=self.fingerprint(root)
            path=root/'uefi/components/os-boot/PianoCpuImageLoan.c';path.write_text(path.read_text()+'\n// new canonical source\n')
            shutil.rmtree(app/'Components/os-boot');shutil.rmtree(app/'core');record=product.prepare_os_boot(root,app)
            shutil.rmtree(staged);shutil.copytree(app,staged)
            manifest=root/'build/product/prepared-manifest.json';prepared=json.loads(manifest.read_text());prepared['os_boot']=record;manifest.write_text(json.dumps(prepared))
            after=self.fingerprint(root);self.assertNotEqual(before['sha256'],after['sha256'])
            self.assertEqual(before['file_count'],after['file_count'])

    def test_unbound_backend_stays_explicit(self):
        status=product.backend_status()['efi_android_linux_boot']
        self.assertEqual(status['shared_os_loader'],'COMPILED_PLATFORM_NOT_READY')
        self.assertFalse(status['platform_bound']);self.assertFalse(status['full_ddr_verified'])
        self.assertEqual(status['source_budget_bytes'],64*1024*1024)

    def test_display_mapping_freshness_and_no_readiness_promotion(self):
        for mutation in ('staged_row','target_row','generator','driver_pin','parser','binding','hardware_ready','missing_row'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory(prefix='product-display-binding-')as tmp:
                root=Path(tmp);app,_,staged=self.fixture(root);before=self.fingerprint(root)
                if mutation in ('staged_row','target_row','missing_row'):
                    table=(staged if mutation!='target_row'else app).parent.parent/'Library/MemoryMapLib/MemoryMapLib.c'
                    source=table.read_text()
                    if mutation=='missing_row':source=source.replace('"Piano_Display_GCC"','"Missing_GCC"')
                    else:source=source.replace('0x1F5000','0x1F4000')
                    table.write_text(source)
                elif mutation in ('generator','driver_pin','parser'):
                    name='tools/piano_display_mapping.py'if mutation=='generator'else 'tools/compose_piano_dtb.py'if mutation=='parser'else 'upstream/linux-piano/drivers/clk/qcom/gcc-sm8750.c'
                    path=root/name;path.write_bytes(path.read_bytes()+b'\n// changed input\n')
                else:
                    path=root/'build/product/prepared-manifest.json';manifest=json.loads(path.read_text())
                    manifest['display_mapping']['hardware_verified'if mutation=='hardware_ready'else 'compiled_into_product']=mutation=='hardware_ready'
                    path.write_text(json.dumps(manifest))
                if mutation=='parser':
                    # Parser dependencies are fingerprinted even though exact
                    # fixed output geometry remains separately reconstructed.
                    self.assertNotEqual(self.fingerprint(root)['sha256'],before['sha256'])
                else:
                    with self.assertRaises(ValueError):self.fingerprint(root)



if __name__=='__main__':unittest.main()
