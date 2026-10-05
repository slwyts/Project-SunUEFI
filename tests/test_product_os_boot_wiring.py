"""Actual product staging/INF and freshness for the shared OS loader, no device."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import prepare_product as product
import build_integrity as integrity


class ProductOsBootWiringTests(unittest.TestCase):
    def fixture(self,root):
        shutil.copytree(ROOT/'bootprofiles/os-boot',root/'bootprofiles/os-boot')
        for family in product.OBSERVATION_FAMILIES:shutil.copytree(ROOT/'bootprofiles'/family,root/'bootprofiles'/family)
        shared=root/'bootprofiles/uefi-app';shared.mkdir(parents=True)
        for name,path in product.shared_boot_headers(ROOT).items():
            target=shared/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        for name in product.SOURCE_NAMES:(shared/name).write_bytes((ROOT/'bootprofiles/uefi-app'/name).read_bytes())
        app=root/'platforms/pianoProductPkg/Applications/ProductCore';app.mkdir(parents=True)
        for name in product.SOURCE_NAMES:shutil.copyfile(shared/name,app/name)
        for name,path in product.shared_boot_headers(root).items():
            target=app/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        record=product.prepare_os_boot(root,app)
        observation=product.prepare_observation_families(root,app)
        (app/'ProductCore.inf').write_text(product.core_inf())
        staged=root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg'
        shutil.copytree(app.parent.parent,staged)
        manifest=root/'build/product/prepared-manifest.json';manifest.parent.mkdir(parents=True);manifest.write_text(json.dumps({'os_boot':record,'dxe_observation':observation}))
        for relative in ('config/piano-product.json','tools/prepare_product.py','tools/build_product.sh','tools/package_product.py',
                         'tools/prepare_product_pump.py','tools/prepare_product_ui.py','tools/prepare_nv_runtime_guard.py','tools/simpleinit_build_identity.py',
                         'tools/build_simpleinit.sh','tools/prepare_simpleinit.py','tools/product_payload_digest.py',
                         'artifacts/simpleinit/product/SimpleInit.efi','artifacts/simpleinit/product/app-payload.bin','artifacts/simpleinit/product/build-ok.json'):
            path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture')
        return app,record,staged/'Applications/ProductCore'

    def fingerprint(self,root):
        with patch('prepare_product_pump.prepare',return_value={'files':{}}),patch('prepare_product_ui.prepare',return_value={'files':{}}),patch('prepare_nv_runtime_guard.prepare',return_value={'files':{}}):
            return integrity.inputs(root,'product')

    def test_actual_staged_sources_compile_from_inf_without_rewriting(self):
        with tempfile.TemporaryDirectory(prefix='product-os-wiring-')as tmp:
            root=Path(tmp);app,record,staged=self.fixture(root)
            self.assertTrue(product.verify_os_boot(root,app,record));self.assertTrue(product.verify_os_boot(root,staged,record))
            self.assertFalse(record['platform_bound']);self.assertFalse(record['full_ddr_verified']);self.assertFalse(record['start_enabled'])
            inf=(staged/'ProductCore.inf').read_text();section=inf.split('[Sources]\n',1)[1].split('[Packages]',1)[0]
            bound=[line.strip()for line in section.splitlines()if line.strip().startswith('OsBoot/')]
            self.assertEqual(bound,list(product.OS_BOOT_INF_SOURCES))
            for name,path in product.os_boot_files(root).items():self.assertEqual((staged/'OsBoot'/name).read_bytes(),path.read_bytes())
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
                    name='PianoBootFileSource.h'if mutation=='canonical_h'else 'PianoBootFileSource.c';path=root/'bootprofiles/os-boot'/name
                    if mutation=='canonical_missing':path.unlink()
                    else:path.write_text(path.read_text()+'\n// changed\n')
                elif mutation in ('staged_c','staged_h'):
                    path=staged/'OsBoot'/('PianoLinuxEfiSession.c'if mutation=='staged_c'else 'PianoLinuxEfiSession.h');path.write_text(path.read_text()+'\n// changed\n')
                elif mutation in ('relative_header','module_header'):
                    path=staged/('uefi-app/PianoFastbootLaunch.h'if mutation=='relative_header'else 'PianoFastbootLaunch.h');path.write_text(path.read_text()+'\n// changed\n')
                else:
                    path=staged/'ProductCore.inf';path.write_text(path.read_text().replace('  OsBoot/PianoCpuImageLoan.c\n'if mutation=='inf_source'else '  SynchronizationLib\n',''))
                with self.assertRaises(ValueError):self.fingerprint(root)
                self.assertGreater(before['file_count'],6)

    def test_coherent_source_reprepare_changes_build_input_fingerprint(self):
        with tempfile.TemporaryDirectory(prefix='product-os-reprepare-')as tmp:
            root=Path(tmp);app,record,staged=self.fixture(root);before=self.fingerprint(root)
            path=root/'bootprofiles/os-boot/PianoCpuImageLoan.c';path.write_text(path.read_text()+'\n// new canonical source\n')
            shutil.rmtree(app/'OsBoot');shutil.rmtree(app/'uefi-app');record=product.prepare_os_boot(root,app)
            shutil.rmtree(staged);shutil.copytree(app,staged)
            manifest=root/'build/product/prepared-manifest.json';prepared=json.loads(manifest.read_text());prepared['os_boot']=record;manifest.write_text(json.dumps(prepared))
            after=self.fingerprint(root);self.assertNotEqual(before['sha256'],after['sha256'])
            self.assertEqual(before['file_count'],after['file_count'])

    def test_unbound_backend_stays_explicit(self):
        status=product.backend_status()['efi_android_linux_boot']
        self.assertEqual(status['shared_os_loader'],'COMPILED_PLATFORM_NOT_READY')
        self.assertFalse(status['platform_bound']);self.assertFalse(status['full_ddr_verified'])
        self.assertEqual(status['source_budget_bytes'],64*1024*1024)


if __name__=='__main__':unittest.main()
