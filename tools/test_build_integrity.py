#!/usr/bin/env python3
"""Verify failed/stale builds are rejected without contacting any device."""
import json
from pathlib import Path
import tempfile
import unittest
from build_integrity import start,finish,validate


class IntegrityTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.platform=self.root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoGuiPkg'
        self.platform.mkdir(parents=True);self.source=self.platform/'App.c';self.source.write_text('v1')
        out=self.root/'artifacts/gui';out.mkdir(parents=True)
        (out/'piano-stage0.fd').write_bytes(b'FD');(out/'BootShim.bin').write_bytes(b'SHIM')
    def tearDown(self):self.tmp.cleanup()
    def test_failed_build_rejects_old_artifacts(self):
        old=start(self.root,'gui');finish(self.root,'gui')
        start(self.root,'gui') # Represents a later build that fails before finish.
        with self.assertRaises(ValueError):validate(self.root,'gui',{'build_id':old['build_id']})
    def test_success_and_old_package(self):
        record=start(self.root,'gui');finish(self.root,'gui')
        self.assertEqual(validate(self.root,'gui',record)['build_id'],record['build_id'])
        with self.assertRaises(ValueError):validate(self.root,'gui',{'build_id':'older'})
    def test_changed_inputs(self):
        start(self.root,'gui');finish(self.root,'gui');self.source.write_text('v2')
        with self.assertRaises(ValueError):validate(self.root,'gui')
    def test_changed_during_build(self):
        start(self.root,'gui');self.source.write_text('v2')
        with self.assertRaises(ValueError):finish(self.root,'gui')
        with self.assertRaises(ValueError):validate(self.root,'gui')
    def test_output_tamper(self):
        start(self.root,'gui');finish(self.root,'gui')
        (self.root/'artifacts/gui/piano-stage0.fd').write_bytes(b'other')
        with self.assertRaises(ValueError):validate(self.root,'gui')


if __name__=='__main__':unittest.main()
