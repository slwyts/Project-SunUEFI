"""Actual DSP/PCI fold, shared parser and service wiring; no device/module access."""
import errno
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apply_piano_dsp_pcie_masters as patch
import check_piano_kernel_contexts as contexts
import stage_piano_ram_hardware as stage
import test_piano_ram_hardware as base
from compose_piano_dtb import read_fdt


class DspPcieTests(unittest.TestCase):
    def setUp(self):
        self.fixture = base.RamHardwareTests('runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.fixture.fixture()
        self.contexts = self.fixture.context_fixture()
        self.sysfs = self.fixture.sysfs

    def directory(self, scope):
        device, node, mode, pairs = contexts.SCOPES[scope]
        if device is None:
            device = 'created-child-' + scope
        return self.sysfs / ('bus/pci/devices' if scope == 'radio' else 'bus/platform/devices') / device

    def test_actual_dynamic_platform_consumers_and_thirteen_rpc_two_audio_pairs(self):
        result = base.hardware.prove(self.fixture.module, 'adsp', self.sysfs, self.contexts)
        self.assertEqual(len(result['contexts']), 6)
        self.assertEqual(sum(len(part['routes']) for part in result['contexts']), 13)
        self.assertEqual(len(contexts.read_scope('audio', self.sysfs)['routes']), 2)
        self.assertEqual(contexts.read_scope('fastrpc-5', self.sysfs)['consumer'], 'created-child-fastrpc-5')
        duplicate = self.sysfs / 'bus/platform/devices/duplicate-audio'
        duplicate.mkdir()
        (duplicate / 'of_node').symlink_to(self.directory('audio') / 'of_node')
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            contexts.read_scope('audio', self.sysfs)
        duplicate.rename(self.fixture.path / 'out-of-platform')
        (self.directory('audio') / 'driver').unlink()
        with self.assertRaises(OSError):
            contexts.read_scope('audio', self.sysfs)

    def test_exact_eagain_identity_fallback_and_no_other_fault_fallback(self):
        directory = self.directory('audio')
        rows = (directory / 'piano_dma_context').read_text().splitlines()
        identity = 'piano-dma-route-v1 domain=identity ids=2\n'
        for row in rows[1:]:
            identity += ' '.join(token for token in row.replace('sctlr=00000001', 'sctlr=00000000').split()
                                  if token.split('=')[0] not in contexts.CONTEXT_FIELDS) + '\n'
        (directory / 'piano_dma_route').write_text(identity)
        original = Path.read_text
        for code in (errno.EAGAIN, errno.EIO, errno.EPERM, errno.ESTALE, errno.EOPNOTSUPP, errno.ENOENT):
            calls = []
            def read(path, *args, **kwargs):
                calls.append(path)
                if path == directory / 'piano_dma_context':
                    raise OSError(code, 'actual getter error fixture')
                return original(path, *args, **kwargs)
            with mock.patch.object(Path, 'read_text', new=read):
                if code == errno.EAGAIN:
                    proof = contexts.read_scope('audio', self.sysfs)
                    self.assertEqual(proof['domain'], 'identity')
                    self.assertFalse(proof['dma_transfer_verified'])
                    self.assertEqual(calls.count(directory / 'piano_dma_route'), 2)
                else:
                    with self.assertRaises(OSError):
                        contexts.read_scope('audio', self.sysfs)
                    self.assertNotIn(directory / 'piano_dma_route', calls)

    def test_real_pci_endpoint_mapping_vendor_and_no_parf_or_dma_claim(self):
        proof = contexts.read_scope('radio', self.sysfs)
        self.assertEqual(proof['consumer'], '0000:01:00.0')
        self.assertEqual(proof['routes'][0]['sid'], '1401')
        self.assertFalse(proof['pci_parf_hardware_table_verified'])
        self.assertFalse(proof['dma_transfer_verified'])
        vendor = self.directory('radio') / 'vendor'
        vendor.write_text('0x1234\n')
        with self.assertRaisesRegex(ValueError, 'WCN7861'):
            contexts.read_scope('radio', self.sysfs)
        vendor.write_text('0x17cb\n')
        mapping = self.sysfs / 'firmware/devicetree/base/soc/pcie@1c00000/iommu-map'
        original = mapping.read_bytes()
        mapping.write_bytes(original[:12] + (0x7f).to_bytes(4, 'big') + original[16:])
        with self.assertRaisesRegex(ValueError, 'RID/SID'):
            contexts.read_scope('radio', self.sysfs)

    def test_all_fifteen_pairs_required_and_legacy_provider_rejected(self):
        cb = self.directory('fastrpc-5') / 'piano_dma_context'
        text = cb.read_text()
        cb.write_text('\n'.join(text.splitlines()[:-1]) + '\n')
        with self.assertRaises(ValueError):
            base.hardware.prove(self.fixture.module, 'adsp', self.sysfs, self.contexts)
        cb.write_text(text)
        node = self.sysfs / ('firmware/devicetree/base' + contexts.SCOPES['audio'][1]) / 'iommus'
        raw = node.read_bytes()
        node.write_bytes((0x7c).to_bytes(4, 'big') + raw[4:])
        with self.assertRaisesRegex(ValueError, 'real IOMMU provider'):
            contexts.read_scope('audio', self.sysfs)

    def test_normal_creation_before_guard_then_card_or_dma_consumers(self):
        scripts = ROOT / 'upstream/debian-piano-current/rootfs/overlay/usr/lib/piano'
        dsp = stage.adapt('adsp-start', (scripts / 'adsp-start').read_bytes()).decode()
        self.assertLess(dsp.index('say "DONE adsp running"'), dsp.index('modprobe fastrpc'))
        self.assertLess(dsp.index('modprobe fastrpc'), dsp.index('--require-scope adsp'))
        self.assertLess(dsp.index('--require-scope adsp'), dsp.index('say "BEGIN power"'))
        audio = stage.adapt('audio-start', (scripts / 'audio-start').read_bytes()).decode()
        self.assertLess(audio.index('for m in snd_q6apm'), audio.index('--require-scope audio'))
        self.assertLess(audio.index('--require-scope audio'), audio.index('say "BEGIN card"'))
        radio = stage.adapt('radio-start', (scripts / 'radio-start').read_bytes()).decode()
        self.assertLess(radio.index('modprobe --ignore-install phy_qcom_qmp_pcie'), radio.index('--require-scope radio'))
        self.assertLess(radio.index('--require-scope radio'), radio.index('modprobe qrtr_mhi'))
        self.assertLess(radio.index('--require-scope radio'), radio.index('modprobe ath12k_wifi7'))
        for text in (dsp, audio, radio):
            self.assertNotIn('devmem', text)
            self.assertNotIn('piano-smmu-ready', text)

    def test_bounded_creation_wait_does_not_retry_fault_or_changed_configuration(self):
        clock = [0]
        calls = []
        def now(): return clock[0]
        def sleep(seconds): clock[0] += seconds
        def read(module, scope):
            calls.append(scope)
            if clock[0] < 2:
                raise OSError(errno.ENOENT, 'normal RPMsg children not created yet')
            return contexts.read_scope('audio', self.sysfs)
        result = base.hardware.wait_proof(None, 'audio', 3, read, now, sleep)
        self.assertEqual(result['domain'], 'stage1')
        self.assertEqual(len(calls), 3)
        for error in (OSError(errno.EIO, 'fault'), ValueError('changed context')):
            calls.clear()
            def fail(module, scope):
                calls.append(scope)
                raise error
            with self.assertRaises(type(error)):
                base.hardware.wait_proof(None, 'audio', 30, fail, now, sleep)
            self.assertEqual(len(calls), 1)

    def test_actual_cpp_dtc_fold_only_eight_properties_and_normalized_rid_tuples(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'private/analysis', prefix='dsp-pcie-fold-test-') as temporary:
            base_path = ROOT / 'private/analysis/piano-linux-managed-clocks-v1/Piano-full-linux-managed-clocks.dtb'
            args = SimpleNamespace(base=base_path, base_sha256=patch.sha(base_path.read_bytes()),
                kernel_tree=ROOT / 'build/kernel-worktrees/piano-smmu-context',
                rom=ROOT / 'private/analysis/android-memory-2026-10-05/live.dtb', output_dir=Path(temporary) / 'out',
                dtc=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc',
                fdtoverlay=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay',
                libfdt=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
            report = patch.fold(args)
            self.assertEqual(len(report['changes']), 8)
            self.assertFalse(report['domain_forced'])
            self.assertFalse(report['hardware_dma_verified'])
            result = read_fdt((args.output_dir / 'Piano-full-linux-managed-dsp-pcie.dtb').read_bytes())
            before = read_fdt(base_path.read_bytes())
            self.assertEqual(before['reservations'], result['reservations'])
            self.assertEqual(before['phandles'], result['phandles'])
            self.assertEqual(len(result['tree'][patch.PCI]['iommu-map']), 40)
            args.base_sha256 = '0' * 64
            with self.assertRaises(ValueError): patch.fold(args)


if __name__ == '__main__':
    unittest.main()
