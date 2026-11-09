"""Exercise the actual early-expansion receipt parser without device access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PianoF2fsGrowTests(unittest.TestCase):
    def test_only_one_confirmed_capacity_and_job_are_accepted(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as folder:
            binary = Path(folder) / 'grow'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(ROOT / 'tests/native/test_piano_f2fs_grow.c'), '-o', str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10,
                           env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
