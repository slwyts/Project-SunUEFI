"""Host command safety/provenance only; never connect to a USB device."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
with patch.object(sys,'path',[str(ROOT/'tools'),*sys.path]):
    spec=importlib.util.spec_from_file_location('ram_boot_host',ROOT/'tools/check_fastboot_ram_boot.py')
    host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
spec=importlib.util.spec_from_file_location("ram_probe_builder_host",ROOT/"tools/build_ram_boot_probe.py")
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)


class RamBootHostTests(unittest.TestCase):
    def fixture(self,repo):
        folder=repo/'artifacts/ram-boot-probe/return-probe-v1'
        builder.build(folder)
        return folder

    def test_dry_run_never_invokes_process_or_creates_test_record(self):
        with tempfile.TemporaryDirectory(prefix='ram-boot-host-') as tmp:
            repo=Path(tmp);self.fixture(repo);output=io.StringIO()
            with patch.object(host,'ROOT',repo),patch.object(sys,'argv',['check','--test-id','90']),patch.object(host.subprocess,'run',side_effect=AssertionError('dry-run contacted device')),contextlib.redirect_stdout(output):
                host.main()
            result=json.loads(output.getvalue())
            self.assertEqual(result['status'],'DRY_RUN_NO_DEVICE_ACTION')
            self.assertEqual(result['serial'],'SunUEFI-piano')
            self.assertFalse((repo/'private').exists())

    def test_execute_requires_matching_firmware_ram_boot_before_device_command(self):
        with tempfile.TemporaryDirectory(prefix='ram-boot-host-') as tmp:
            repo=Path(tmp);self.fixture(repo)
            with patch.object(host,'ROOT',repo),patch.object(sys,'argv',['check','--test-id','90','--execute']),patch.object(host.subprocess,'run',side_effect=AssertionError('missing boot contacted device')),self.assertRaises(ValueError):
                host.main()

    def test_changed_probe_refused_before_device_command(self):
        with tempfile.TemporaryDirectory(prefix='ram-boot-host-') as tmp:
            repo=Path(tmp);folder=self.fixture(repo)
            probe=folder/'PianoRamBootProbe.efi';data=bytearray(probe.read_bytes());data[-1]^=1;probe.write_bytes(data)
            with patch.object(host,'ROOT',repo),patch.object(sys,'argv',['check','--test-id','90']),patch.object(host.subprocess,'run',side_effect=AssertionError('changed probe contacted device')),self.assertRaises(ValueError):
                host.main()


if __name__=='__main__':unittest.main()
