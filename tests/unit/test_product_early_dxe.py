"""Preserve the captured early DXE bytes and reject changed input identity."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from prepare_product import EARLY_DXE_SOURCE, EARLY_DXE_NAMES, BOOT_FIRMWARE_CODE
from prepare_product import bind_boot_firmware_code, early_dxe_files, stage_early_dxe


class EarlyDxeTests(unittest.TestCase):
    def test_resident_code_permissions_follow_the_cpu_policy(self):
        native='ArmPkg/Drivers/CpuDxe/CpuDxe.inf'
        platform=ROOT/'uefi/platforms/pianoProbePkg'
        with tempfile.TemporaryDirectory() as folder:
            target=Path(folder)
            shutil.copyfile(platform/'pianoProbe.fdf',target/'pianoProduct.fdf')
            shutil.copyfile(platform/'pianoProbe.dsc',target/'pianoProduct.dsc')
            bind_boot_firmware_code(target)
            fdf=(target/'pianoProduct.fdf').read_text()
            priority,volume=fdf.split('  APRIORI DXE {',1)[1].split('  }',1)
            for text in (priority,volume):
                entries=[line.split('INF ',1)[1].strip() for line in text.splitlines() if 'INF ' in line]
                self.assertEqual(entries[entries.index(native)+1],BOOT_FIRMWARE_CODE)
            self.assertEqual((target/'pianoProduct.dsc').read_text().count(BOOT_FIRMWARE_CODE),1)

    def test_stages_original_inf_pe_and_depex_as_early_dxe(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            shutil.copytree(ROOT/EARLY_DXE_SOURCE,root/EARLY_DXE_SOURCE)
            stage_early_dxe(root)
            target=root/'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced'
            for name in EARLY_DXE_NAMES:
                self.assertEqual((target/name).read_bytes(),(ROOT/EARLY_DXE_SOURCE/name).read_bytes())
            self.assertEqual(set(early_dxe_files(root)),
                             {root/EARLY_DXE_SOURCE/name for name in (*EARLY_DXE_NAMES,'provenance.json')})
            self.assertFalse((root/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation').exists())

    def test_rejects_changed_source_or_provenance_before_staging(self):
        for name in (*EARLY_DXE_NAMES,'provenance.json'):
            with self.subTest(name=name),tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                shutil.copytree(ROOT/EARLY_DXE_SOURCE,root/EARLY_DXE_SOURCE)
                path=root/EARLY_DXE_SOURCE/name
                path.write_bytes(path.read_bytes()+b'changed')
                with self.assertRaises(ValueError):stage_early_dxe(root)
                self.assertFalse((root/'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced').exists())


if __name__=='__main__':unittest.main()
