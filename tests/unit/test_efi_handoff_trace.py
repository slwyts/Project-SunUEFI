import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class EfiTraceSourceTests(unittest.TestCase):
    def test_actual_source_under_asan_ubsan_and_unreadable_boot_services(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "efi-trace-test"
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "tests/native"),
                str(ROOT / "tests/native/test_efi_handoff_trace.c"), "-lz", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(0, build.returncode, build.stderr)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1")
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("post-EBS PROT_NONE passed", result.stdout)

if __name__ == "__main__":
    unittest.main()
