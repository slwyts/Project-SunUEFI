"""Product assembly declarations and actual shared source wiring, no device."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import prepare_product as product
from product_contract import validate,validate_build_manifest


class ProductBuilderTests(unittest.TestCase):
    def test_actual_product_sources_not_diagnostic_entry_apps(self):
        for name in product.SOURCE_NAMES:
            self.assertTrue((ROOT/'bootprofiles/uefi-app'/name).is_file(),name)
        inf=product.core_inf()
        self.assertIn('ENTRY_POINT = PianoProductCoreEntry',inf)
        self.assertIn('PianoProductOwners.c',inf)
        self.assertNotIn('RamApp.c',inf)
        for forbidden in ('PianoUsbUfsFetch.c','PianoUsbRamBoot.c','PianoUfsWriteTest.c','PianoDmaSelfTest.c'):
            self.assertNotIn(forbidden,inf)
        for required in ('PIANO_USB_SERVICE=1','PIANO_USB_EP0=1','PIANO_USB_FASTBOOT=1','PIANO_USB_SCREENSHOT=1','PIANO_USB_UFS_FETCH=1','PIANO_USB_RAM_BOOT=1','PIANO_UFS_BLOCKIO=1'):
            self.assertIn(required,product.PRODUCT_FLAGS)

    def test_enabled_contract_does_not_claim_incomplete_backends_ready(self):
        contract=validate(json.loads((ROOT/'config/piano-product.json').read_text()))
        backends=product.backend_status()
        self.assertEqual(set(contract['features']),set(backends))
        self.assertEqual(backends['persistent_variables']['status'],'RAM_ONLY')
        self.assertEqual(backends['ufs_blockio_read_write']['status'],'RESERVED_VOLUME_BACKEND_UNPROVISIONED')
        self.assertEqual(backends['ufs_blockio_read_write']['original_media'],'READ_ONLY')
        for feature in ('pogo_keyboard_touchpad','touchscreen','usb_host'):
            self.assertEqual(backends[feature]['status'],'NOT_READY')
        self.assertEqual(backends['usb_device_fastboot']['current_download_limit_bytes'],64*1024*1024)
        self.assertEqual(backends['usb_device_fastboot']['target_download_limit_bytes'],1024*1024*1024)

    def test_product_bds_actual_source_has_logo_single_core_and_no_reboot_timer(self):
        source=(ROOT/'bootprofiles/product-support/Library/ProductBootManagerLib/ProductBootManagerLib.c').read_text()
        self.assertIn('PianoProductDrawSplash(Gop,SplashAlive,&Report)',source)
        self.assertIn('gEfiGraphicsOutputProtocolGuid',source)
        self.assertIn('gBS->StartImage(Core',source)
        self.assertIn('35E0D1B5',source)
        for forbidden in ('SetTimer','TimerRelative','ResetSystem','UnloadImage','RamAppEntry'):
            self.assertNotIn(forbidden,source)

    def test_actual_prepared_manifest_if_present_has_all_shared_bindings(self):
        path=ROOT/'build/product/prepared-manifest.json'
        if not path.exists():self.skipTest('product source not prepared')
        data=json.loads(path.read_text());contract=validate(json.loads((ROOT/'config/piano-product.json').read_text()))
        validate_build_manifest(contract,data)
        self.assertEqual(data['status'],'INCOMPLETE_NOT_RELEASE')
        self.assertEqual(set(data['backends']),set(contract['features']))
        self.assertTrue(data['simpleinit']['product_gui_pump'])
        dsc=(ROOT/'platforms/pianoProductPkg/pianoProduct.dsc').read_text()
        self.assertIn('LibraryClasses.common.DXE_CORE',dsc)
        self.assertIn('PianoProductPumpLib/PianoProductPumpLib.inf',dsc)
        self.assertIn('ProductCore/ProductCore.inf',dsc)


if __name__=='__main__':unittest.main()
