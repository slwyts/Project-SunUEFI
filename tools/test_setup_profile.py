#!/usr/bin/env python3
"""Validate setup module/library/PCD metadata without preparing/building a FV."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('prepare_gui_profile',ROOT/'tools/prepare_gui_profile.py')
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'

class SetupMetadataTests(unittest.TestCase):
    def test_standard_engine_protocol_matches_browser(self):
        browser=(BASE/profile.SETUP_FV_MODULES[0]).read_text()
        display=(BASE/profile.SETUP_FV_MODULES[1]).read_text()
        self.assertIn('gEdkiiFormDisplayEngineProtocolGuid',browser)
        self.assertIn('gEdkiiFormDisplayEngineProtocolGuid',display)
        self.assertNotIn('MsGraphicsPkg',profile.SETUP_DSC_ADDITIONS)
        self.assertIn('SetupBrowserDxe/SetupBrowserDxe.inf',(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc').read_text())
    def test_library_paths_and_dependencies_exist(self):
        additions=profile.SETUP_DSC_ADDITIONS
        paths=re.findall(r'\|([^\s]+\.inf)',additions)
        for path in paths:self.assertTrue((BASE/path).is_file(),path)
        for name in ('CustomizedDisplayLib','FileExplorerLib','DeviceManagerUiLib','BootManagerUiLib','BootMaintenanceManagerUiLib'):
            self.assertIn(name,additions)
    def test_dynamic_console_pcds_and_native_gop(self):
        additions=profile.SETUP_DSC_ADDITIONS
        dynamic=additions.split('[PcdsDynamicDefault]',1)[1].split('[',1)[0]
        self.assertIn('PcdConOutColumn|80',dynamic);self.assertIn('PcdConOutRow|25',dynamic)
        self.assertIn('PcdSetupVideoHorizontalResolution|3200',dynamic)
        self.assertIn('PcdSetupVideoVerticalResolution|2136',dynamic)
        self.assertIn('PcdEmuVariableNvModeEnable|TRUE',additions)
    def test_uiapp_guid_and_real_smbios_fallback(self):
        ui=(BASE/profile.SETUP_FV_MODULES[2]).read_text()
        self.assertIn('462CAA21-7614-4503-836E-8AB6F4662331',ui)
        source=(BASE/'MdeModulePkg/Application/UiApp/FrontPage.c').read_text()
        self.assertIn('Smbios protocol not found, get the default value',source)
        self.assertNotIn('SmbiosDxe',profile.SETUP_DSC_ADDITIONS)
    def test_no_flash_variable_backend(self):
        self.assertNotIn('FaultTolerantWrite',profile.SETUP_DSC_ADDITIONS)
        self.assertNotIn('Fvb',profile.SETUP_DSC_ADDITIONS)
        source=(ROOT/'bootprofiles/uefi-app/PianoLaunchSetup.c').read_text()
        self.assertIn('PcdGetBool(PcdEmuVariableNvModeEnable)',source)
        self.assertNotIn('SetVariable(',source)

if __name__=='__main__':unittest.main()
