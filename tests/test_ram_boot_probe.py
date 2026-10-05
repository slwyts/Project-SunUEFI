"""Build the actual AA64 PE and test probe C on host; no device execution."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
spec=importlib.util.spec_from_file_location('ram_probe_builder',ROOT/'tools/build_ram_boot_probe.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)


class RamBootProbeTests(unittest.TestCase):
    def test_actual_c_volatile_record_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='ram-probe-tests-') as tmp:
            binary=str(Path(tmp)/'probe')
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(INC),'-I',str(INC/'X64'),
                str(ROOT/'tests/PianoRamBootProbeTest.c'),'-o',binary],check=True)
            subprocess.run([binary],check=True)

    def test_actual_pe_is_deterministic_and_parser_accepted(self):
        with tempfile.TemporaryDirectory(prefix='ram-probe-build-tests-') as tmp:
            a=builder.build(Path(tmp)/'a');b=builder.build(Path(tmp)/'b')
            self.assertEqual(a['sha256'],b['sha256'])
            self.assertEqual(a['status'],'BUILT_NOT_DEVICE_EXECUTED')
            self.assertEqual(a['variable_attributes'],2)
            self.assertFalse(a['nonvolatile_variable_writes'])
            self.assertGreater(a['relocation_bytes'],0)
            self.assertIn('machine=aa64 subsystem=10',a['parser_output'])
            with self.assertRaises(ValueError):builder.build(Path(tmp)/'a')


if __name__=='__main__':unittest.main()
