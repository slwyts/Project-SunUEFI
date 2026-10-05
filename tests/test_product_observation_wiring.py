"""Real flat collector/guard compilation and product family freshness checks."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest
import test_product_os_boot_wiring as os_wiring
ROOT=os_wiring.ROOT;product=os_wiring.product


class ProductObservationWiringTests(unittest.TestCase):
    def fixture(self,root):
        fixture=os_wiring.ProductOsBootWiringTests();app,_,staged=fixture.fixture(root)
        record=json.loads((root/'build/product/prepared-manifest.json').read_text())['dxe_observation']
        return fixture,app,staged,record

    def test_generated_flat_sources_compile_in_actual_inf_layout(self):
        with tempfile.TemporaryDirectory(prefix='product-observe-')as tmp:
            root=Path(tmp);fixture,app,staged,record=self.fixture(root)
            self.assertTrue(product.verify_observation_families(root,app,record));self.assertTrue(product.verify_observation_families(root,staged,record))
            for family,rows in product.observation_files(root).items():
                for name,path in rows.items():self.assertEqual(path.read_bytes(),(staged/name).read_bytes());self.assertEqual(product.sha(path),record['families'][family][name])
            inf=(staged/'ProductCore.inf').read_text();sources={line.strip()for line in inf.split('[Sources]\n',1)[1].split('[Packages]',1)[0].splitlines()}
            expected=(*product.OBSERVATION_INF_SOURCES,'PianoProductSmem.c');self.assertTrue(set(expected)<=sources)
            include=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
            for name in expected:
                if name.endswith('.c'):
                    output=root/(Path(name).stem+'.obj')
                    subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-c','-Wall','-Wextra','-Werror','-I',str(include),'-I',str(include/'AArch64'),'-I',str(staged),str(staged/name),'-o',str(output)],check=True)
                    self.assertGreater(output.stat().st_size,0)
            fixture.fingerprint(root)

    def test_freshness_rejects_family_source_copy_header_and_inf_changes(self):
        for family,names in product.OBSERVATION_FAMILIES.items():
            for mutation in ('canonical_c','canonical_h','missing','target_c','upstream_c','upstream_h','inf_c','inf_h','event_guid','cpu_protocol'):
                with self.subTest(family=family,mutation=mutation),tempfile.TemporaryDirectory(prefix='product-observe-fresh-')as tmp:
                    root=Path(tmp);fixture,app,staged,record=self.fixture(root);fixture.fingerprint(root)
                    if mutation.startswith('canonical_')or mutation=='missing':
                        path=root/'bootprofiles'/family/names[1 if mutation=='canonical_h'else 0]
                        if mutation=='missing':path.unlink()
                        else:path.write_text(path.read_text()+'\n// changed\n')
                    elif mutation in ('target_c','upstream_c','upstream_h'):
                        path=(app if mutation=='target_c'else staged)/names[1 if mutation=='upstream_h'else 0];path.write_text(path.read_text()+'\n// changed\n')
                    else:
                        token=names[0 if mutation=='inf_c'else 1]if mutation.startswith('inf_')else 'gEfiEventExitBootServicesGuid'if mutation=='event_guid'else 'gEfiCpuArchProtocolGuid'
                        path=staged/'ProductCore.inf';path.write_text(path.read_text().replace('  '+token+'\n',''))
                    with self.assertRaises(ValueError):fixture.fingerprint(root)

    def test_coherent_family_reprepare_changes_fingerprint(self):
        for family,names in product.OBSERVATION_FAMILIES.items():
            with self.subTest(family=family),tempfile.TemporaryDirectory(prefix='product-observe-reprepare-')as tmp:
                root=Path(tmp);fixture,app,staged,record=self.fixture(root);before=fixture.fingerprint(root)
                path=root/'bootprofiles'/family/names[0];path.write_text(path.read_text()+'\n// updated canonical source\n')
                record=product.prepare_observation_families(root,app);shutil.rmtree(staged);shutil.copytree(app,staged)
                manifest=root/'build/product/prepared-manifest.json';prepared=json.loads(manifest.read_text());prepared['dxe_observation']=record;manifest.write_text(json.dumps(prepared))
                self.assertNotEqual(before['sha256'],fixture.fingerprint(root)['sha256'])

    def test_metadata_does_not_promote_observation_to_memory_permission(self):
        with tempfile.TemporaryDirectory(prefix='product-observe-status-')as tmp:
            _,_,_,record=self.fixture(Path(tmp));self.assertTrue(record['platform_bound']);self.assertFalse(record['device_validated'])
            for key in ('sec_early_ready','high_ddr_mapped','memory_ownership_granted'):self.assertFalse(record[key])
            status=product.backend_status()['dma_smmu'];self.assertEqual(status['smem_observation'],'READ_ONLY_DXE_BOUND_UNTESTED')
            self.assertFalse(status['sec_early_ready']);self.assertFalse(status['high_ddr_mapped']);self.assertFalse(status['high_ram_ownership_verified'])


if __name__=='__main__':unittest.main()
