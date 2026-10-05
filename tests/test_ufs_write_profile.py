"""Pure profile argument/string checks; no main, prepare, staging or device calls."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('prepare_gui_profile', ROOT / 'tools/prepare_gui_profile.py')
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)
RAM_APP = ROOT / 'upstream/Mu-Silicium/Platforms/Xiaomi/pianoGuiPkg/Applications/RamApp/RamApp.c'
WRITE_MODES = ('--ufs-write-preflight', '--ufs-write-restore-test')
ISOLATED_FROM = (
    '--ufs-filesystems', '--ufs-shell', '--ufs-shell-interactive', '--ufs-setup',
    '--usb-controller', '--usb-ep0', '--usb-debug', '--touch-probe', '--gpi-probe',
    '--fault-recovery-test', '--ram-qupfw', '--qupfw-disk', '--pmic-metadata',
)


class UfsWriteProfileOptionsTests(unittest.TestCase):
    def parse(self, flags=()):
        parser = profile.argument_parser()
        args = parser.parse_args(flags)
        return parser, args

    def test_default_both_write_flags_are_false_and_validation_does_not_stage(self):
        parser, args = self.parse()
        self.assertFalse(args.ufs_write_preflight)
        self.assertFalse(args.ufs_write_restore_test)
        self.assertFalse(args.ufs_blockio)
        before = vars(args).copy()
        with (mock.patch.object(profile, 'main', side_effect=AssertionError('main must not run')),
              mock.patch.object(profile, 'load_write_attestation', side_effect=AssertionError('no implicit attestation')),
              mock.patch.object(shutil, 'copytree', side_effect=AssertionError('no staging')),
              mock.patch.object(shutil, 'copyfile', side_effect=AssertionError('no staging'))):
            profile.validate_write_options(parser, args)
        self.assertEqual(before, vars(args))

    def test_parser_rejects_both_write_flags_in_either_order(self):
        for flags in (WRITE_MODES, tuple(reversed(WRITE_MODES))):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    self.parse(flags)
                self.assertEqual(2, error.exception.code)

    def test_each_write_mode_implies_only_readonly_blockio_at_validation(self):
        for mode in WRITE_MODES:
            parser, args = self.parse((mode,))
            self.assertFalse(args.ufs_blockio)
            before = vars(args).copy()
            profile.validate_write_options(parser, args)
            before['ufs_blockio'] = True
            with self.subTest(mode=mode):
                self.assertEqual(before, vars(args))
                self.assertEqual(mode == '--ufs-write-preflight', args.ufs_write_preflight)
                self.assertEqual(mode == '--ufs-write-restore-test', args.ufs_write_restore_test)

    def test_each_write_mode_rejects_all_conflicting_consumers(self):
        for mode in WRITE_MODES:
            for consumer in ISOLATED_FROM:
                parser, args = self.parse((mode, consumer))
                with self.subTest(mode=mode, consumer=consumer), contextlib.redirect_stderr(io.StringIO()) as stderr:
                    with self.assertRaises(SystemExit) as error:
                        profile.validate_write_options(parser, args)
                    self.assertEqual(2, error.exception.code)
                    self.assertIn('requires an isolated profile', stderr.getvalue())
                    self.assertFalse(args.ufs_blockio)

    def test_existing_consumers_do_not_implicitly_enable_a_write_mode(self):
        for consumer in ISOLATED_FROM:
            parser, args = self.parse((consumer,))
            before = vars(args).copy()
            profile.validate_write_options(parser, args)
            with self.subTest(consumer=consumer):
                self.assertEqual(before, vars(args))
                self.assertFalse(args.ufs_write_preflight)
                self.assertFalse(args.ufs_write_restore_test)

    def test_required_ufs_foundation_keys_and_recovery_options_remain_compatible(self):
        dependencies = (
            '--ufs-blockio', '--ufs-dma-nop', '--dma-owned', '--dma-probe', '--ufs-probe',
            '--foundation', '--keys', '--fault-recovery', '--return-seconds', '120',
        )
        for mode in WRITE_MODES:
            parser, args = self.parse((mode, *dependencies))
            before = vars(args).copy()
            profile.validate_write_options(parser, args)
            with self.subTest(mode=mode):
                self.assertEqual(before, vars(args))
                self.assertTrue(args.ufs_blockio)
                self.assertEqual(120, args.return_seconds)


class UfsWriteRamAppTests(unittest.TestCase):
    @unittest.skipUnless(RAM_APP.is_file(), 'existing upstream RamApp template absent')
    def test_template_loses_simpleinit_launch_and_keeps_probe_keys_and_resident_wait(self):
        # Staging is mutable between hardware profiles. Build the launch
        # fixture from the canonical source, then add the pre-launch providers
        # whose lifetime the write-only profile must preserve.
        original = (ROOT/'bootprofiles/uefi-app/RamApp.c').read_text().replace(
            '  Status=gBS->LoadImage (FALSE,ImageHandle',
            '  PianoProbeUfs(Fdt);\n  PianoStartKeys(Fdt);\n  Status=gBS->LoadImage (FALSE,ImageHandle')
        staged_before = RAM_APP.read_text()
        modified_before = RAM_APP.stat().st_mtime_ns
        self.assertIn('PianoProbeUfs(Fdt);', original)
        self.assertIn('PianoStartKeys(Fdt);', original)
        for symbol in ('LoadImage', 'StartImage', 'UnloadImage'):
            self.assertIn(symbol, original)
        with (mock.patch.object(Path, 'write_text', side_effect=AssertionError('no staging writes')),
              mock.patch.object(Path, 'write_bytes', side_effect=AssertionError('no staging writes'))):
            result = profile.write_test_ram_app(original)
        for symbol in ('LoadImage', 'StartImage', 'UnloadImage', 'SUNUEFI_SIMPLEINIT_LOAD', 'SUNUEFI_SIMPLEINIT_RETURN'):
            self.assertNotIn(symbol, result)
        for symbol in ('PianoProbeUfs(Fdt);', 'PianoStartKeys(Fdt);', 'ProbeGop ();',
                       'FdtCheckHeader (Fdt)', 'KnownRam (', 'Sha256HashAll (', 'SUNUEFI_RAM_APP_HASH_OK'):
            self.assertIn(symbol, result)
        self.assertIn('SUNUEFI_UFS_WRITE_PROFILE_WAIT boot_blocked=1 readonly_blockio=1', result)
        self.assertIn('while(TRUE){gBS->Stall(100000);}', result)
        self.assertLess(result.index('PianoProbeUfs(Fdt);'), result.index('PianoStartKeys(Fdt);'))
        self.assertLess(result.index('PianoStartKeys(Fdt);'), result.index('SUNUEFI_UFS_WRITE_PROFILE_WAIT'))
        self.assertTrue(result.endswith('while(TRUE){gBS->Stall(100000);}\n}\n'))
        self.assertEqual(staged_before, RAM_APP.read_text())
        self.assertEqual(modified_before, RAM_APP.stat().st_mtime_ns)

    def test_missing_or_duplicate_launch_anchor_is_refused(self):
        anchor = '  Status=gBS->LoadImage (FALSE,ImageHandle'
        for text in ('EFI_STATUS Entry(VOID){return EFI_SUCCESS;}\n', anchor + '\n' + anchor):
            with self.subTest(anchors=text.count(anchor)), self.assertRaisesRegex(ValueError, 'SimpleInit launch anchor'):
                profile.write_test_ram_app(text)


class UfsWriteAttestationLoadingTests(unittest.TestCase):
    def test_attestation_import_only_has_no_main_prepare_verify_or_staging_effect(self):
        # Loading defines functions and pins. Verification and preparation are
        # deliberately not called here; those have separate PC fixture tests.
        with (mock.patch.object(profile, 'main', side_effect=AssertionError('no main')),
              mock.patch.object(shutil, 'copytree', side_effect=AssertionError('no staging')),
              mock.patch.object(shutil, 'copyfile', side_effect=AssertionError('no staging')),
              mock.patch.object(Path, 'write_text', side_effect=AssertionError('no output')),
              mock.patch.object(Path, 'write_bytes', side_effect=AssertionError('no output')),
              mock.patch.object(Path, 'read_bytes', side_effect=AssertionError('no archive verification during import'))):
            attestation = profile.load_write_attestation(ROOT)
        self.assertTrue(callable(attestation.verify_capture))
        self.assertTrue(callable(attestation.prepare_ufs_write_test))
        self.assertEqual('PianoUfsWriteTestBaseline.h', attestation.HEADER_NAME)
        self.assertEqual(ROOT / 'build/ufs-write-test/PianoUfsWriteTestBaseline.h', attestation.OUTPUT)
        self.assertEqual(9, len(attestation.EXPECTED_FILES))

    def test_missing_attestation_module_is_refused_before_any_staging(self):
        missing = ROOT / 'nonexistent-attestation-root-for-offline-test'
        self.assertFalse(missing.exists())
        with (mock.patch.object(shutil, 'copytree', side_effect=AssertionError('no staging')),
              mock.patch.object(shutil, 'copyfile', side_effect=AssertionError('no staging'))):
            with self.assertRaises(FileNotFoundError):
                profile.load_write_attestation(missing)
        self.assertFalse(missing.exists())


if __name__ == '__main__':
    unittest.main()
