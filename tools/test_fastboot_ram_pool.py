#!/usr/bin/env python3
"""Offline interval/DRAM ownership audit tests; no hardware."""
import json
import struct
import unittest
from audit_fastboot_ram_pool import audit, interval, merge, subtract, trace
from compose_piano_dtb import write_fdt


def reg(base, size):
    return struct.pack('>QQ', base, size)


class AuditTest(unittest.TestCase):
    def test_ranges(self):
        self.assertEqual(merge([(10, 20), (5, 10), (12, 13), (30, 30)]), [(5, 20)])
        self.assertEqual(subtract([(0, 100)], [(0, 10), (20, 30), (25, 40), (90, 120)]), [(10, 20), (40, 90)])
        self.assertEqual(interval((1 << 64) - 1, 1), ((1 << 64) - 1, 1 << 64))
        with self.assertRaises(ValueError):
            interval((1 << 64) - 1, 2)

    def test_trace(self):
        line = 'PIANO_EFI_TRACE EBS_ENTER known=1 bytes=48 stride=48\nPIANO_EFI_TRACE MAP index=0 type=7 phys=0x80000000 virt=0x0 pages=0x100 attr=0x8\n'
        self.assertEqual(len(trace(line)[0]), 1)
        for bad in (line.replace('known=1', 'known=0'), line.replace('index=0', 'index=1'), line.replace('bytes=48', 'bytes=96')):
            with self.assertRaises(ValueError):
                trace(bad)

    def test_cma_and_dynamic_are_distinct(self):
        props = {'#address-cells': struct.pack('>I', 2), '#size-cells': struct.pack('>I', 2)}
        tree = {'/': props, '/memory': {'device_type': b'memory\0', 'reg': reg(0x80000000, 0x80000000) + reg(0xa00000000, 0x80000000)},
                '/reserved-memory': {**props, 'ranges': b''},
                '/reserved-memory/trust': {'reg': reg(0xf3800000, 0x5000000), 'reusable': b'', 'compatible': b'shared-dma-pool\0'},
                '/reserved-memory/dump': {'size': struct.pack('>Q', 0x5a00000), 'alloc-ranges': reg(0x100000000, 0xfffffffeffffffff)}}
        dtb = write_fdt({'tree': tree, 'reservations': [], 'boot_cpu': 0})
        efi = 'PIANO_EFI_TRACE EBS_ENTER known=1 bytes=48 stride=48\nPIANO_EFI_TRACE MAP index=0 type=7 phys=0xbd930000 virt=0x0 pages=0x100 attr=0x8\n'
        result = audit(dtb, [{'base': 0x80000000, 'size': 0x80000000}], efi)
        self.assertEqual(len(result['one_gib_candidates']), 2)
        self.assertFalse(result['one_gib_candidates'][0]['safe_to_use'])
        self.assertEqual(result['one_gib_candidates'][0]['unresolved_dynamic_owners'], ['/reserved-memory/dump'])
        self.assertEqual(result['strict_free_after_all_dynamic_alloc_windows'], [])
        cma = result['fixed_reusable_cma_contract'][0]
        self.assertFalse(cma['no_map'])
        self.assertEqual(cma['outside_efi_linux_usable'][0]['start'], '0xf3800000')
        self.assertIn('must retain valid System RAM', result['linux_contract_note'])
        runtime = {'phase': 'android-runtime-fixture', 'regions': [{'node': '/reserved-memory/dump', 'start': '0xa00000000', 'bytes': 0x5a00000}]}
        observed = audit(dtb, [{'base': 0x80000000, 'size': 0x80000000}], efi, runtime)
        self.assertEqual(observed['one_gib_candidates'][0]['observed_phase_conflicts'], ['/reserved-memory/dump'])
        self.assertFalse(observed['one_gib_candidates'][0]['safe_to_use'])
        self.assertTrue(observed['free_after_observed_phase_reserves'])
        runtime['phase'] = ''
        with self.assertRaises(ValueError):
            audit(dtb, [], efi, runtime)


if __name__ == '__main__':
    unittest.main()
