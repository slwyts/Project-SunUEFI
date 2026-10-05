import importlib.util
from pathlib import Path
import unittest
import zlib

spec=importlib.util.spec_from_file_location('owned_diag_decoder',
    Path(__file__).resolve().parents[1]/'tools/decode_owned_smmu_diag.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)


def record(body,mirror=False):
    return (b'SUNUEFI_SMMU_OWNED_DIAG'+(b'_COPY' if mirror else b'')+b' '+body+
            f' crc32={zlib.crc32(body):08X}\n'.encode())


class DiagnosticDecoderTests(unittest.TestCase):
    def test_intact_mirror_selected_without_modifying_raw(self):
        body=b'phase=close-rejected seq=12 owner=0 idx=1'
        bad=record(body).replace(b'idx=1',b'idx=2')
        raw=bad+record(body,True);before=raw[:]
        result=decoder.decode(raw)
        self.assertEqual(raw,before);self.assertEqual(result['invalid_copies'],1)
        self.assertEqual(result['records'][0]['fields']['idx'],'1')

    def test_identical_valid_copies_counted(self):
        body=b'phase=baseline-final seq=8 owner=0 idx=113'
        result=decoder.decode(record(body)+record(body,True))
        self.assertEqual(result['records'][0]['valid_copies'],2)

    def test_conflicting_valid_body_refused(self):
        with self.assertRaises(ValueError):
            decoder.decode(record(b'phase=close-rejected seq=8 idx=1')+
                           record(b'phase=close-rejected seq=8 idx=113',True))

    def test_no_valid_or_missing_sequence_refused(self):
        for raw in (b'noise only',record(b'phase=close-rejected idx=1')):
            with self.assertRaises(ValueError):decoder.decode(raw)


if __name__=='__main__':unittest.main()
