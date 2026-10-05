"""Exercise actual prepare in a temporary repo; no build/device/current staging."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('diagnostic_profiles',ROOT/'tools/prepare_gui_profile.py')
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)


class NewDiagnosticProfileTests(unittest.TestCase):
    def test_isolation_before_staging(self):
        for flag in ('--pogo-register-probe','--high-ram-readonly'):
            for other in ('--usb-ram-boot','--usb-fastboot','--ufs-blockio','--ufs-write-restore-test',
                          '--fault-recovery','--keys','--touch-probe'):
                parser=profile.argument_parser();args=parser.parse_args([flag,other])
                with self.subTest(flag=flag,other=other),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                    profile.validate_readonly_diagnostic_options(parser,args)
        parser=profile.argument_parser();args=parser.parse_args(['--usb-ram-boot'])
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):profile.validate_usb_fastboot_options(parser,args)

    def test_actual_temporary_profile_generation(self):
        for flag in ('--pogo-register-probe','--high-ram-readonly','--usb-ram-boot'):
            with self.subTest(flag=flag),tempfile.TemporaryDirectory(prefix='sunuefi-profile-') as temporary:
                repo=Path(temporary);(repo/'tools').mkdir();(repo/'build').mkdir()
                # This fixture validates generated source/flags, not native FV
                # loading; it must not depend on an ignored prior build file.
                (repo/'build/native-foundation.fdf.inc').write_text('  // host-only fixture: no native binaries\n')
                shutil.copytree(ROOT/'platforms/pianoProbePkg',repo/'platforms/pianoProbePkg')
                shutil.copytree(ROOT/'bootprofiles/uefi-app',repo/'bootprofiles/uefi-app')
                with patch.object(profile,'__file__',str(repo/'tools/prepare_gui_profile.py')),patch.object(sys,'argv',['prepare',flag,'--return-seconds','120']),contextlib.redirect_stdout(io.StringIO()):
                    profile.main()
                app=repo/'platforms/pianoGuiPkg/Applications/RamApp'
                text=(app/'RamApp.c').read_text();inf=(app/'RamApp.inf').read_text()
                self.assertNotIn('Status=gBS->LoadImage (FALSE,ImageHandle',text)
                self.assertNotIn('PianoStartKeys(Fdt)',text)
                self.assertNotIn('PianoProbeUfs(Fdt)',text)
                cfg=json.loads((repo/'build/gui-profile.json').read_text())
                if flag=='--pogo-register-probe':
                    self.assertIn('PianoProbePogo(Fdt)',text)
                    self.assertNotIn('PianoFaultRecovery.c',inf)
                    self.assertIn('PianoGeniI2cPio.c',inf)
                    self.assertTrue((app/'PianoPogoProbe.c').read_text().startswith('#define PIANO_POGO_PROBE_EXPERIMENT 1'))
                elif flag=='--high-ram-readonly':
                    self.assertIn('PianoProbeHighRamReadonly()',text)
                    self.assertNotIn('NativeProbe.c',inf)
                    self.assertIn('PianoFaultRecovery.c',inf)
                    self.assertFalse(cfg['foundation'])
                else:
                    self.assertIn('PianoRunUsbRamBoot(Fdt,ImageHandle)',text)
                    self.assertTrue((app/'PianoFastboot.c').read_text().startswith('#define PIANO_USB_RAM_BOOT 1'))
                    self.assertIn('PianoFastbootDownloadBlob.c',inf)
                    self.assertNotIn('PianoUfsDma.c',inf)
                    timer=(repo/'platforms/pianoGuiPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.c').read_text()
                    self.assertIn('Usb->Halt()!=EFI_SUCCESS',timer)
                    self.assertFalse(cfg['ufs_blockio'])


if __name__=='__main__':unittest.main()
