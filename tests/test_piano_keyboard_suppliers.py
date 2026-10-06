"""Actual sealed keyboard DTB repair and complete fold-chain admission."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apply_piano_keyboard_suppliers as keyboard
import assemble_piano_linux as assemble
from compose_piano_dtb import read_fdt, write_fdt

BASE = ROOT / 'private/analysis/piano-linux-managed-dsp-pcie-v4/Piano-full-linux-managed-dsp-pcie.dtb'
BASE_SHA = 'b8065721bbd9bf86b025129fbe2d1099f461607fa50cff9600de7c11a2e8768f'
LIB = ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1'
MANIFESTS = [ROOT / 'private/analysis' / name / 'manifest.json' for name in (
    'piano-full-dtb-fd6266-impact-fixed', 'piano-linux-owned-dma',
    'piano-linux-managed-clocks-v1', 'piano-linux-managed-dsp-pcie-v4')]


class KeyboardSuppliersTests(unittest.TestCase):
    def test_actual_fold_preserves_supplies_and_full_chain_accepts_only_exact_delta(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'private/analysis', prefix='keyboard-fold-test-') as temporary:
            args = SimpleNamespace(base=BASE, base_sha256=BASE_SHA,
                kernel_tree=ROOT / 'build/kernel-worktrees/piano-smmu-context',
                output_dir=Path(temporary) / 'out', libfdt=LIB)
            report = keyboard.fold(args)
            target = args.output_dir / keyboard.FILENAME
            result = read_fdt(target.read_bytes())
            before = read_fdt(BASE.read_bytes())
            keyboard.validate_delta(before, result)
            self.assertNotIn('panel', result['tree'][keyboard.KEYBOARD])
            for key in keyboard.SUPPLIES:
                self.assertEqual(result['tree'][keyboard.KEYBOARD][key], before['tree'][keyboard.KEYBOARD][key])
            self.assertFalse(report['keyboard_hardware_verified'])
            accepted = assemble.device_tree(target, MANIFESTS + [args.output_dir / 'manifest.json'], LIB)
            self.assertEqual(accepted['sha256'], report['output_sha256'])
            # Hash and manifest agree, but an unrelated GPIO edit must still fail admission.
            result['tree'][keyboard.KEYBOARD]['reset-gpios'] = before['tree'][keyboard.KEYBOARD]['sleep-gpios']
            altered = write_fdt(result)
            target.write_bytes(altered)
            report['output_sha256'] = keyboard.sha(altered)
            report['output_bytes'] = len(altered)
            report['changes'].append({'path': keyboard.KEYBOARD, 'property': 'reset-gpios'})
            (args.output_dir / 'manifest.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'unaudited keyboard'):
                assemble.device_tree(target, MANIFESTS + [args.output_dir / 'manifest.json'], LIB)
            args.base_sha256 = '0' * 64
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                keyboard.fold(args)

    def test_supply_and_legacy_panel_identity_drift_are_rejected(self):
        original = read_fdt(BASE.read_bytes())
        for path, key, value in (
                (keyboard.KEYBOARD, 'panel', original['tree'][keyboard.KEYBOARD]['panel'][:4]),
                (keyboard.KEYBOARD, 'dvdd-supply', original['tree'][keyboard.KEYBOARD]['vddio-supply']),
                (keyboard.PMIC, 'qcom,pmic-id', b'd\0'),
                (keyboard.PMIC + '/ldo14', 'regulator-min-microvolt', keyboard.cell(3300000))):
            with self.subTest(path=path, property=key):
                broken = copy.deepcopy(original)
                broken['tree'][path][key] = value
                with self.assertRaises(ValueError):
                    keyboard.repair(broken)

    def test_restored_panel_or_unrelated_supplier_edit_are_rejected(self):
        before = read_fdt(BASE.read_bytes())
        for path, key in ((keyboard.KEYBOARD, 'panel'), (keyboard.PMIC + '/ldo14', 'regulator-name')):
            with self.subTest(path=path, property=key):
                after = keyboard.repair(before)
                after['tree'][path][key] = before['tree'][path][key] if key == 'panel' else b'unknown-rail\0'
                with self.assertRaisesRegex(ValueError, 'unrelated DTB'):
                    keyboard.validate_delta(before, after)


if __name__ == '__main__':
    unittest.main()
