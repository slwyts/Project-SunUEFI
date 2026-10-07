"""Actual runtime compilation and exact kernel/stage refusal, no live root edit."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build_piano_runtime_helpers as runtime


class PianoRuntimeBuilderTests(unittest.TestCase):
    def test_actual_canonical_compile_help_topology_and_module(self):
        if not(ROOT/'build/distros/piano-runtime-build/usr/lib/aarch64-linux-gnu/libc.a').exists():self.skipTest('Verified native ARM64 sysroot not prepared')
        with tempfile.TemporaryDirectory(prefix='runtime-build-test-',dir=ROOT/'build')as directory:
            output=Path(directory)/'bundle';args=runtime.parser().parse_args(['--output',str(output)])
            manifest=runtime.build(args);self.assertEqual(len(manifest['runtime_files']),5);self.assertFalse(manifest['hardware_verified'])
            self.assertEqual(manifest['kernel']['release'],runtime.DEFAULT_RELEASE)
            for name in runtime.PUBLIC_SOURCES:self.assertTrue(runtime.verify_elf(output/name)['static_no_pt_interp'])
            module=output/'v4l2loopback/v4l2loopback.ko';self.assertEqual(runtime.modinfo(module)['vermagic'].split()[0],runtime.DEFAULT_RELEASE)
            with self.assertRaisesRegex(ValueError,'preserve prior'):runtime.build(args)
            destination=Path(directory)/'unused-stage'
            with self.assertRaisesRegex(ValueError,'derived'):runtime.stage(output,destination)

    def test_actual_kernel_release_mismatch_and_entry_help_branch(self):
        source=ROOT/'build/kernel-worktrees/piano-full-integration';build=ROOT/'build/kernels/full-integration'
        if not(build/'Module.symvers').exists():self.skipTest('Full candidate kernel not built')
        with self.assertRaisesRegex(ValueError,'release differs'):runtime.kernel_identity(build,source,runtime.UAPI_COMMIT,'7.2.6-wrong')
        identity=runtime.kernel_identity(build,source,runtime.UAPI_COMMIT,runtime.DEFAULT_RELEASE);self.assertEqual(identity['commit'],runtime.UAPI_COMMIT)
        for name in ('piano-camerad','piano-pd-locator'):
            text=runtime.entry_source(name);self.assertIn('PianoOriginalMain(void)',text)
            self.assertLess(text.index('"--help"'),text.index('return PianoOriginalMain()'))
        with self.assertRaises(ValueError):runtime.entry_source('untrusted-program')


if __name__=='__main__':unittest.main()
