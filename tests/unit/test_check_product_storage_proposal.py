"""Actual proposal verification rejects repinned, structurally changed media."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import check_product_storage_proposal as verify

PROPOSAL = ROOT/'private/provisioning/piano-storage-v1-final'


@unittest.skipUnless(PROPOSAL.is_dir(), 'Private concrete proposal absent')
class ProposalVerifierTests(unittest.TestCase):
    def test_current_proposal_and_latest_readonly_capture(self):
        result = verify.check(PROPOSAL, ROOT/'private/captures/ufs-test-area-5')
        self.assertEqual(result['status'], 'EXACT_PC_PROPOSAL_VALIDATED_NO_DEVICE_ACTION')
        self.assertFalse(result['permanent_reservation_authorized'])

    def test_manifest_repin_does_not_authorize_changed_bytes(self):
        for name, offset in (('primary-entries.bin', 0), ('volume-header.bin', 80),
                             ('PianoUEFI-storage.img', 2816*4096+767*4096),
                             ('nv-snapshot.bin', 4096)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary)/'proposal'
                shutil.copytree(PROPOSAL, copied)
                target = copied/name
                changed = bytearray(target.read_bytes())
                changed[offset] ^= 1
                target.write_bytes(changed)
                manifest = json.loads((copied/'manifest.json').read_text())
                manifest['proposal_files'][name] = {'bytes': len(changed),
                                                    'sha256': hashlib.sha256(changed).hexdigest()}
                (copied/'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):
                    verify.check(copied)

    def test_manifest_authorization_flag_and_duplicate_keys_refused(self):
        for mutation in ('authorization', 'duplicate'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary)/'proposal'
                shutil.copytree(PROPOSAL, copied)
                path = copied/'manifest.json'
                if mutation == 'authorization':
                    data = json.loads(path.read_text())
                    data['device_writes_authorized'] = True
                    path.write_text(json.dumps(data))
                else:
                    text = path.read_text()
                    path.write_text(text.replace('{', '{"schema":1,', 1))
                with self.assertRaises(ValueError):
                    verify.check(copied)


if __name__ == '__main__':
    unittest.main()
