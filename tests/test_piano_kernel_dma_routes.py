"""Actual route ABI parser and actual six-master overlay; no device reads."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import check_piano_kernel_dma_routes as checker
import apply_piano_dma_masters as patch


class KernelDmaRouteTests(unittest.TestCase):
    def text(self, mode=0, origin='kernel-installed'):
        s2cr = 0x000100ff if mode else 3
        return ('piano-dma-route-v1 domain=identity ids=1\n'
                f'sid=40 mask=0 slot=2 origin={origin} smr=80000040 '
                f's2cr={s2cr:08x} expected={s2cr:08x} cb={255 if mode else 3} '
                'sctlr=00000000 cbar=00010000 fsr=00000400\n')

    def test_actual_kernel_identity_encodings_and_adopted_origin(self):
        self.assertEqual(checker.verify(self.text(), 0x40)['verified'], 'kernel-private-and-hardware-readback')
        self.assertEqual(checker.verify(self.text(mode=1), 0x40)['origin'], 'kernel-installed')
        self.assertEqual(checker.verify(self.text(origin='firmware-adopted'), 0x40)['origin'], 'firmware-adopted')
        self.assertEqual(checker.verify(self.text(origin='firmware-adopted'), 0x40)['ownership'], 'kernel-attached-inherited-bypass')

    def test_fake_marker_unbound_unknown_fields_and_wrong_master_rejected(self):
        for raw in ('ready\n', '', self.text().replace('domain=identity', 'domain=dma'),
                    self.text().replace('sid=40', 'sid=41'), self.text().replace('mask=0', 'mask=1'),
                    self.text().replace('origin=kernel-installed', 'origin=guessed')):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                checker.verify(raw, 0x40)
        for raw in (self.text().replace('mask=0', 'mask=0 sid=40'),
                    self.text().replace('smr=80000040', 'smr=180000040'),
                    self.text().replace('smr=80000040', 'smr=0x80000040'),
                    self.text().replace('slot=2', 'slot=-1'),
                    self.text().replace('cb=3', 'cb=+3')):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                checker.verify(raw, 0x40)

    def test_stale_hardware_translation_fault_and_privilege_mismatch_rejected(self):
        for raw in (self.text().replace('smr=80000040', 'smr=00000040'),
                    self.text().replace('sctlr=00000000', 'sctlr=00000001'),
                    self.text().replace('fsr=00000400', 'fsr=00000402'),
                    self.text().replace('expected=00000003', 'expected=01000003'),
                    self.text().replace('s2cr=00000003', 's2cr=00020003')):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                checker.verify(raw, 0x40)

    def test_actual_overlay_retargets_only_six_known_master_properties(self):
        base = ROOT / 'private/analysis/piano-full-dtb-fd6266-impact-fixed/Piano-full-camera.dtb'
        kernel = ROOT / 'build/kernel-worktrees/piano-smmu-routes'
        if not base.exists() or not kernel.exists():
            self.skipTest('actual private folded input/kernel worktree required')
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(base=base, base_sha256='b03853bb56cd38fcacc296f8824ecb4432154d7aebc04403fabe9ba04455b3c4',
                                   kernel_tree=kernel, output_dir=Path(directory),
                                   dtc=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc',
                                   fdtoverlay=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay',
                                   libfdt=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
            result = patch.apply(args)
            self.assertEqual(len(result['changes']), 6)
            self.assertEqual({x['property'] for x in result['changes']}, {'iommus'})
            self.assertEqual({x['sid'] for x in result['masters']}, {0x40, 0x60, 0xb6, 0xa3, 0x436, 0x423})
            self.assertFalse(result['hardware_verified'])
            self.assertFalse(result['memory_ownership_authorized'])


if __name__ == '__main__':
    unittest.main()
