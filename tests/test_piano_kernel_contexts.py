"""Actual kernel stage1 comparator and strict readback consumer, no device."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import check_piano_kernel_contexts as context


class KernelContextTests(unittest.TestCase):
    def report(self, pairs, mode='stage1', mask_override=None):
        kind = 'context' if mode == 'stage1' else 'route'
        lines = [f'piano-dma-{kind}-v1 domain={mode} ids={len(pairs)}']
        for index, (sid, mask) in enumerate(pairs):
            actual_mask = mask if mask_override is None else mask_override
            smr = 0x80000000 | (actual_mask << 16) | sid
            line = (f'sid={sid:x} mask={mask:x} slot={index} origin=kernel-installed '
                    f'smr={smr:08x} s2cr=00000003 expected=00000003 cb=3 '
                    f'sctlr={1 if mode=="stage1" else 0:08x} cbar=00010000 fsr=00000400')
            if mode == 'stage1':
                line += (' ttbr0=00070000bd980000 ttbr1=0007000000000000 '
                         'tcr=00800000 tcr2=00070014 mair0=0000ff44 mair1=00000000')
            lines.append(line)
        return '\n'.join(lines) + '\n'

    def test_real_shared_c_register_comparator_under_sanitizers(self):
        source = ROOT / 'build/kernel-worktrees/piano-smmu-context/tools/testing/selftests/iommu/qcom-handoff-snapshot.c'
        if not source.exists():self.skipTest('Actual kernel context worktree missing')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='context-c-') as directory:
            exe = Path(directory) / 'test'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-pie', '-no-pie', str(source), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertIn('stage1 register comparator', result.stdout)

    def test_multi_stream_stage1_and_prebind_masked_identity(self):
        pairs = [(0x1940, 0), (0x1947, 0)]
        self.assertEqual(len(context.verify(self.report(pairs), 'stage1', pairs)), 2)
        mdss = [(0x800, 2)]
        value = context.verify(self.report(mdss, 'identity', 15), 'identity', mdss)
        self.assertFalse(value[0]['dma_transfer_verified'])
        self.assertEqual(context.SCOPES['mdss'][2], 'stage1')
        self.assertEqual(context.SCOPES['mdss-prebind'][2], 'identity')

    def test_fake_marker_bad_mode_context_fault_and_inconsistent_bank_rejected(self):
        pairs = [(0, 0), (1, 0)]; original = self.report(pairs)
        bad = ['ready\n', original.replace('domain=stage1', 'domain=identity'),
               original.replace('origin=kernel-installed', 'origin=firmware-adopted'),
               original.replace('fsr=00000400', 'fsr=00000402'),
               original.replace('sctlr=00000001', 'sctlr=00000000'),
               original.replace('ttbr0=00070000bd980000', 'ttbr0=0007000000000000'),
               original.replace('ttbr0=00070000bd980000', 'ttbr0=70000bd980000'),
               original.replace('sid=0 mask=0', 'sid=0 mask=0 sid=0'),
               original.replace('tcr=00800000', 'tcr=00800080'),
               original.replace('smr=80000001', 'smr=80000002')]
        lines = original.splitlines();lines[-1] = lines[-1].replace('mair0=0000ff44', 'mair0=00000044')
        bad.append('\n'.join(lines) + '\n')
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):context.verify(text, 'stage1', pairs)

    def test_actual_sysfs_layout_node_and_provider_validation(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='context-sysfs-') as directory:
            sysfs = Path(directory);device, name, mode, pairs = context.SCOPES['video']
            node = sysfs / ('firmware/devicetree/base' + name);node.mkdir(parents=True)
            provider = sysfs / 'firmware/devicetree/base/soc/iommu@15000000';provider.mkdir()
            handle = bytes.fromhex('000008f7');(provider/'phandle').write_bytes(handle)
            (provider/'#iommu-cells').write_bytes(b'\0\0\0\2')
            raw = b''.join(handle+sid.to_bytes(4,'big')+mask.to_bytes(4,'big') for sid,mask in pairs)
            (node/'iommus').write_bytes(raw)
            dev = sysfs/'bus/platform/devices'/device;dev.mkdir(parents=True)
            (dev/'of_node').symlink_to(node)
            (dev/'piano_dma_context').write_text(self.report(pairs))
            result=context.read_scope('video',sysfs);self.assertFalse(result['marker_written'])
            (node/'iommus').write_bytes(b'\0'*len(raw))
            with self.assertRaisesRegex(ValueError,'real IOMMU provider'):context.read_scope('video',sysfs)


if __name__ == '__main__':unittest.main()
