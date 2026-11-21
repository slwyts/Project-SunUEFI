"""Exercise the Windows miniport's UFSHCI command and scatter/gather descriptors."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'windows/drivers/ufs'


class UfsTransferTest(unittest.TestCase):
    def test_scsi_wire_format_dma_ranges_and_responses(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / 'ufs-transfer'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', SRC,
                            ROOT / 'tests/native/UfsTransferTest.c', SRC / 'transfer.c', '-o', exe], check=True)
            subprocess.run([exe], check=True)


if __name__ == '__main__':
    unittest.main()
