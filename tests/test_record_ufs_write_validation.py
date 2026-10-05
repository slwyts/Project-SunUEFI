import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('ufs_write_record',
                                            ROOT / 'tools/record_ufs_write_validation.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

REPORT = '''SUNUEFI_UFS_WRITE_REPORT_BEGIN mode=restore-test started=1 returned=1 executing=0 lun=4 lba=375040 bytes=4096 write_doorbells=2 sync_doorbells=2
SUNUEFI_UFS_WRITE_RESULT outcome=1 gate=Success first_failure=Success final=Success attempts=2 test_match=1 restore_match=1 restored_verified=1 data_unchanged=1 safe_to_continue=1 requires_recovery=0 quarantined=0
SUNUEFI_UFS_WRITE_ATTEMPT id=0 attempted=1 returned=1 restore=0 lun=4 lba=375040 bytes=4096 transferred=4096 status=Success
SUNUEFI_UFS_WRITE_ATTEMPT id=1 attempted=1 returned=1 restore=1 lun=4 lba=375040 bytes=4096 transferred=4096 status=Success
SUNUEFI_UFS_WRITE_HASH offset=0 test=7ACC23A1439E0B8A restore=AD7FACB2586FC6E9
SUNUEFI_UFS_WRITE_HASH offset=8 test=B56DB9ED45450309 restore=66C004D7D1D16B02
SUNUEFI_UFS_WRITE_HASH offset=16 test=0A9D34D365207F2D restore=4F5805FF7CB47C7A
SUNUEFI_UFS_WRITE_HASH offset=24 test=CA13BAC5F6416DEE restore=85DABD8B48892CA7
SUNUEFI_UFS_WRITE_REPORT_END boot_blocked=1 complete_os_handoff_verified=0
'''


class WriteReportTests(unittest.TestCase):
    def test_success(self):
        self.assertEqual(module.check_report(REPORT)[1],
                         module.EXPECTED_FILES['first-block-original.bin'][1])

    def test_failed_or_incomplete_report(self):
        changes = [('outcome=1', 'outcome=2'), ('requires_recovery=0', 'requires_recovery=1'),
                   ('write_doorbells=2', 'write_doorbells=1'), ('transferred=4096', 'transferred=0'),
                   ('restore=1 lun=4', 'restore=0 lun=4'), ('lba=375040', 'lba=375041'),
                   ('AD7FACB2586FC6E9', 'BD7FACB2586FC6E9'), ('offset=24', 'offset=16'),
                   ('complete_os_handoff_verified=0', 'complete_os_handoff_verified=1')]
        for old, new in changes:
            with self.subTest(old=old), self.assertRaises(ValueError):
                module.check_report(REPORT.replace(old, new))

    def test_missing_duplicate_and_nonhex_hashes(self):
        for log in (REPORT.split('\n', 1)[1], REPORT + REPORT.splitlines()[0] + '\n',
                    REPORT.replace('CA13BAC5F6416DEE', 'INVALID5F6416DEE')):
            with self.assertRaises(ValueError):
                module.check_report(log)


if __name__ == '__main__':
    unittest.main()
