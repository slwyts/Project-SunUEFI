"""Real libfdt/dtc/fdtoverlay + runtime remap tests, never device access."""
import copy
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('piano_full_dtb', ROOT / 'tools/build_piano_full_dtb.py')
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)
import piano_dtb_impact as impact
DTC = ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc'
FDTO = DTC.with_name('fdtoverlay')
LIB = ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1'
CELL = lambda *n: struct.pack('>' + 'I' * len(n), *n)


class FullDtbRuntimeTests(unittest.TestCase):
    def model(self):
        return {'tree': {'/': {'#address-cells': CELL(2), '#size-cells': CELL(2)},
                         '/chosen': {'bootargs': b'captured\0'},
                         '/memory': {'device_type': b'memory\0', 'reg': CELL(0, 0x80000000, 0, 0x10000000)},
                         '/reserved-memory': {'#address-cells': CELL(2), '#size-cells': CELL(2), 'ranges': b''},
                         '/reserved-memory/adsp': {'phandle': CELL(1), 'reg': CELL(0, 0x90000000, 0, 0x10000)},
                         '/adsp': {'phandle': CELL(2), 'memory-region': CELL(1)},
                         '/observer': {'qcom,rproc-handle': CELL(2), 'numeric-data': CELL(2, 1)}},
                'reservations': [(0x90000000, 0x10000)], 'boot_cpu': 0}

    def parsed(self, model):
        return tool.read_fdt(tool.write_fdt(model))

    def test_actual_libfdt_rejects_bad_header(self):
        blob = tool.write_fdt(self.model())
        self.assertEqual(tool.libcheck(blob, LIB)['tree'], self.model()['tree'])
        bad = bytearray(blob)
        struct.pack_into('>I', bad, 4, len(blob) + 16)
        with self.assertRaises(ValueError):
            tool.libcheck(bytes(bad), LIB)

    def test_runtime_memory_and_refs_survive_different_folded_phandles(self):
        rom, live, folded = self.model(), self.model(), self.model()
        live['tree']['/memory']['reg'] = CELL(0, 0x80000000, 2, 0x10000000)
        live['tree']['/observer']['qcom,rproc-handle'] = CELL(3)
        live['tree']['/new-adsp'] = {'phandle': CELL(3), 'memory-region': CELL(1)}
        live['tree']['/chosen'].update({'kaslr-seed': b'12345678', 'rng-seed': b'abcdefgh',
                                       'linux,initrd-start': CELL(0, 0x92000000), 'linux,initrd-end': CELL(0, 0x93000000)})
        folded['tree']['/reserved-memory/adsp']['phandle'] = CELL(11)
        folded['tree']['/adsp']['phandle'] = CELL(12)
        folded['tree']['/adsp']['memory-region'] = CELL(11)
        folded['tree']['/observer']['qcom,rproc-handle'] = CELL(12)
        folded['tree']['/chosen']['linux,initrd-start'] = CELL(99)
        result, changes = tool.runtime_backfill(self.parsed(rom), self.parsed(live), self.parsed(folded))
        self.assertEqual(result['tree']['/memory']['reg'], live['tree']['/memory']['reg'])
        self.assertEqual(result['tree']['/new-adsp']['phandle'], CELL(13))
        self.assertEqual(result['tree']['/new-adsp']['memory-region'], CELL(11))
        self.assertEqual(result['tree']['/observer']['qcom,rproc-handle'], CELL(13))
        self.assertEqual(result['tree']['/observer']['numeric-data'], CELL(2, 1))
        self.assertTrue(tool.TRANSIENT.isdisjoint(result['tree']['/chosen']))
        self.assertEqual(result['reservations'], live['reservations'])
        self.assertTrue(changes)

    def test_resource_provider_cells_supplies_and_unresolved_are_exposed(self):
        model = self.model()
        model['tree']['/clock'] = {'phandle': CELL(4), '#clock-cells': CELL(1)}
        model['tree']['/consumer'] = {'clocks': CELL(4, 9), 'vdd-supply': CELL(1)}
        result = tool.resources(self.parsed(model))
        self.assertFalse(result['errors'])
        model['tree']['/consumer']['clocks'] = CELL(4)
        errors = tool.resources(self.parsed(model))['errors']
        self.assertEqual(errors[0]['reason'], 'provider cell count mismatch')
        model['tree']['/consumer']['clocks'] = CELL(99, 9)
        self.assertEqual(tool.resources(self.parsed(model))['errors'][0]['reason'], 'unresolved phandle')

    def test_opaque_values_are_never_guessed_as_phandles(self):
        model = self.parsed(self.model())
        self.assertIsNone(tool.reference_offsets(model, '/observer', 'opaque-string', b'ABC'))
        self.assertIsNone(tool.reference_offsets(model, '/observer', 'numeric-data', CELL(1, 2)))

    def test_old_embedded_wiring_is_restored_but_explicit_functional_binding_kept(self):
        rom, prefix, folded = self.model(), self.model(), self.model()
        rom['tree']['/adsp'].update({'reg': CELL(0x3000000, 0x10000), 'compatible': b'vendor,adsp\0'})
        prefix['tree']['/adsp'].update({'reg': CELL(0x6800000, 0x10000), 'compatible': b'old,adsp\0'})
        folded['tree']['/adsp'].update({'reg': CELL(0x6800000, 0x10000), 'compatible': b'mainline,adsp\0'})
        operations = {('/adsp', 'compatible')}
        drift = tool.stock_drift(self.parsed(rom), self.parsed(prefix), operations)
        by_key = {x['property']: x for x in drift}
        self.assertEqual(by_key['reg']['category'], 'reg-address-layout')
        self.assertEqual(by_key['reg']['action'], 'restore-current-ROM')
        self.assertEqual(by_key['compatible']['action'], 'retain-explicit-public-functional-operation')
        result, _ = tool.runtime_backfill(self.parsed(prefix), self.parsed(rom), self.parsed(folded), operations)
        self.assertEqual(result['tree']['/adsp']['reg'], rom['tree']['/adsp']['reg'])
        self.assertEqual(result['tree']['/adsp']['compatible'], b'mainline,adsp\0')

    @unittest.skipUnless(DTC.exists() and FDTO.exists(), 'real compiler tools required')
    def test_real_overlay_once_and_complete_chain_stock_version_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            base = self.model()
            base['tree']['/__symbols__'] = {'adsp': b'/adsp\0'}
            (p / 'base.dtb').write_bytes(tool.write_fdt(base))
            text = '/dts-v1/; /plugin/; / { fragment@0 { target=<&adsp>; __overlay__ { test-property=<7>; }; }; %s };'
            for name, extra in [('stock', ''), ('full', 'fragment@200 { target-path="/"; __overlay__ { added { compatible="test,device"; }; }; };')]:
                (p / (name + '.dts')).write_text(text % extra)
                tool.execute([DTC, '-@', '-I', 'dts', '-O', 'dtb', '-o', p / (name + '.dtbo'), p / (name + '.dts')])
            evidence = tool.stock_evidence(tool.libcheck((p / 'full.dtbo').read_bytes(), LIB), tool.libcheck((p / 'stock.dtbo').read_bytes(), LIB))
            self.assertTrue(evidence['captured_stock_entry_exact'])
            tool.execute([FDTO, '-i', p / 'base.dtb', '-o', p / 'fold.dtb', p / 'full.dtbo'])
            folded = tool.libcheck((p / 'fold.dtb').read_bytes(), LIB)
            self.assertEqual(folded['tree']['/adsp']['test-property'], CELL(7))
            self.assertEqual(folded['tree']['/added']['compatible'], b'test,device\0')
            prefix = tool.stock_prefix(tool.libcheck((p / 'full.dtbo').read_bytes(), LIB))
            self.assertEqual(tool.fragments(prefix), ['/fragment@0'])
            operations = tool.additional_operations(tool.libcheck((p / 'full.dtbo').read_bytes(), LIB),
                                                    tool.libcheck((p / 'base.dtb').read_bytes(), LIB), folded)
            self.assertEqual(operations, {('/added', 'compatible')})
            stock = tool.libcheck((p / 'stock.dtbo').read_bytes(), LIB)
            stock['tree']['/fragment@0/__overlay__']['test-property'] = CELL(8)
            evidence = tool.stock_evidence(tool.libcheck((p / 'full.dtbo').read_bytes(), LIB), stock)
            self.assertFalse(evidence['captured_stock_entry_exact'])
            self.assertEqual(evidence['canonical_different'], ['/fragment@0'])

    def test_input_pin_failure_precedes_external_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input'
            path.write_bytes(b'unexpected')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                tool.pinned(path, '0' * 64)

    def test_helper_match_is_not_a_registered_driver_and_disabled_ancestor_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            kernel = Path(directory)
            (kernel / 'drivers').mkdir()
            (kernel / 'sound').mkdir()
            (kernel / 'drivers/test.c').write_text('''
static const struct of_device_id helper[] = {
 { .compatible = "vendor,legacy" },
};
static const struct of_device_id real[] = {
 { .compatible = "mainline,real" },
};
static struct platform_driver driver = { .driver = { .of_match_table = of_match_ptr(real) } };
''')
            drivers, helpers, _ = impact.driver_matches(kernel, {'vendor,legacy', 'mainline,real'})
            self.assertFalse(drivers.get('vendor,legacy'))
            self.assertTrue(helpers['vendor,legacy'])
            self.assertTrue(drivers['mainline,real'])
            tree = {'/': {}, '/bus': {'status': b'disabled\0'}, '/bus/node': {'status': b'okay\0'}}
            self.assertEqual(impact.available(tree, '/bus/node'), (False, '/bus'))

    @unittest.skipUnless(DTC.exists() and FDTO.exists(), 'real compiler tools required')
    def test_actual_uart_overlay_fixes_exact_ICC_without_provider_or_status_mutation(self):
        kernel = ROOT / 'build/kernel-worktrees/piano-full-integration'
        if not (kernel / 'include/dt-bindings/interconnect/qcom,sm8750-rpmh.h').exists():
            self.skipTest('selected kernel headers required')
        model = self.model()
        parent = '/soc/qcom,qupv3_1_geni_se@ac0000'
        target = parent + '/qcom,qup_uart@a9c000'
        model['tree'].update({'/soc': {}, parent: {},
                             target: {'compatible': b'qcom,geni-debug-uart\0', 'reg': CELL(0xa9c000, 0x4000),
                                      'status': b'okay\0', 'clocks': CELL(1, 102), 'interconnects': CELL(99, 1)},
                             '/icc0': {'phandle': CELL(10), '#interconnect-cells': CELL(2)},
                             '/icc1': {'phandle': CELL(11), '#interconnect-cells': CELL(2)},
                             '/icc2': {'phandle': CELL(12), '#interconnect-cells': CELL(2)},
                             '/__symbols__': {'clk_virt': b'/icc0\0', 'gem_noc': b'/icc1\0', 'config_noc': b'/icc2\0'}})
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(resource_overlay=ROOT / 'configs/linux/dtb/piano-uart7-icc.dtso',
                                   cpp='cpp', kernel_tree=kernel, dtc=DTC, fdtoverlay=FDTO, libfdt=LIB)
            result, evidence = tool.correct_uart_icc(args, Path(directory), self.parsed(model), Path(directory))
            self.assertEqual(tool.reference_offsets(result, target, 'interconnects', result['tree'][target]['interconnects']), [0, 3, 6, 9])
            self.assertEqual(result['tree'][target]['status'], b'okay\0')
            self.assertEqual(result['tree'][target]['clocks'], model['tree'][target]['clocks'])
            self.assertEqual(result['tree']['/icc0'], model['tree']['/icc0'])
            self.assertEqual({x['property'] for x in evidence['property_changes']}, {'interconnects', 'interconnect-names'})


if __name__ == '__main__':
    unittest.main()
