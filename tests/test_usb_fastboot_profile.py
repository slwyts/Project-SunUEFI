"""Profile isolation gates; no staging, build or device operations."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('gui_fastboot_profile',
        Path(__file__).resolve().parents[1]/'tools/prepare_gui_profile.py')
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)


class FastbootProfileTests(unittest.TestCase):
    def test_default_and_old_ep0_do_not_enable_fastboot(self):
        for flags in ([],['--usb-ep0']):
            parser=profile.argument_parser();args=parser.parse_args(flags)
            before=vars(args).copy();profile.validate_usb_fastboot_options(parser,args)
            self.assertFalse(args.usb_fastboot);self.assertEqual(before,vars(args))

    def test_fastboot_implies_ep0_at_validation(self):
        parser=profile.argument_parser();args=parser.parse_args(['--usb-fastboot'])
        profile.validate_usb_fastboot_options(parser,args)
        self.assertTrue(args.usb_ep0)
        self.assertFalse(args.usb_debug)

    def test_rejects_mixed_hardware_consumers_before_staging(self):
        for conflict in ('--ufs-probe','--dma-owned','--ufs-dma-nop','--ufs-blockio',
                         '--ufs-filesystems','--ufs-shell','--ufs-setup','--ufs-write-preflight',
                         '--ufs-write-restore-test','--usb-debug','--touch-probe','--gpi-probe',
                         '--ram-qupfw','--qupfw-disk','--pmic-metadata','--fault-recovery-test'):
            parser=profile.argument_parser();args=parser.parse_args(['--usb-fastboot',conflict])
            with self.subTest(conflict=conflict),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                profile.validate_usb_fastboot_options(parser,args)
            self.assertFalse(args.usb_ep0)


if __name__=='__main__':unittest.main()
