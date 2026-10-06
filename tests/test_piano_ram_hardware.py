"""Actual runtime/adapter source against bounded sysfs fixtures, never hardware."""
import copy
import importlib.machinery
import importlib.util
import json
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import stage_piano_ram_hardware as stage
import apply_piano_tcsr_clocks as clocks
import stage_piano_full_userspace as full_stage
from compose_piano_dtb import read_fdt

loader = importlib.machinery.SourceFileLoader('hardware', str(ROOT / 'bootprofiles/linux-userspace/piano-ram-hardware-prepare'))
spec = importlib.util.spec_from_loader(loader.name, loader)
hardware = importlib.util.module_from_spec(spec)
loader.exec_module(hardware)


class RamHardwareTests(unittest.TestCase):
    def test_disk_root_requires_actual_device_and_partition_identity(self):
        import os, stat
        uid = 'ffc480ed-c219-400b-a8f9-5f6805aa1f34'
        proc = self.path / 'proc'; proc.mkdir()
        (proc / 'cmdline').write_text('piano.root=PARTUUID=' + uid)
        (proc / 'mounts').write_text('/dev/sda36 / ext4 rw 0 0\n')
        (self.run / 'piano-root-mode').write_text('PARTUUID=' + uid)
        (self.run / 'piano-root-device').write_text('/dev/sda36')
        read = Path.read_text
        def text(path, *args, **kwargs):
            if str(path) == '/etc/piano/root-policy.json':
                return json.dumps({'root_partuuid': uid})
            return read(path, *args, **kwargs)
        with mock.patch.object(Path, 'read_text', text), \
             mock.patch.object(Path, 'is_dir', return_value=True), \
             mock.patch.object(hardware.os, 'stat', side_effect=lambda path: SimpleNamespace(st_mode=stat.S_IFBLK, st_rdev=42, st_dev=42)), \
             mock.patch.object(hardware.subprocess, 'check_output', return_value=uid+'\n') as query:
            hardware.require_ram(proc, self.run)
            query.assert_called_once_with(['/usr/sbin/blkid', '-s', 'PARTUUID', '-o', 'value', '/dev/sda36'], text=True)
            query.return_value = 'different\n'
            with self.assertRaises(hardware.Refused): hardware.require_ram(proc, self.run)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='ram-hardware-test-')
        self.path = Path(self.temp.name)
        self.sysfs = self.path / 'sys'
        self.run = self.path / 'run'
        self.run.mkdir()
        self.module = hardware.checker(ROOT / 'tools/check_piano_kernel_dma_routes.py')

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self):
        for device, sid in self.module.DEVICES.items():
            node = self.sysfs / ('firmware/devicetree/base' + self.module.NODES[device])
            node.mkdir(parents=True, exist_ok=True)
            dev = self.sysfs / 'bus/platform/devices' / device
            dev.mkdir(parents=True, exist_ok=True)
            (dev / 'of_node').symlink_to(node)
            (dev / 'piano_dma_route').write_text(
                'piano-dma-route-v1 domain=identity ids=1\n'
                f'sid={sid:x} mask=0 slot=2 origin=kernel-installed smr={0x80000000 | sid:08x} '
                's2cr=00000003 expected=00000003 cb=3 sctlr=00000000 cbar=00010000 fsr=00000400\n')
        # Actual folded DT properties, not a second hand-coded clock binding.
        real = ROOT / 'private/analysis/piano-linux-managed-clocks-v1/Piano-full-linux-managed-clocks.dtb'
        if not real.exists():
            self.skipTest('actual clock candidate required')
        tree = read_fdt(real.read_bytes())['tree']
        for path in (clocks.REAL, clocks.WRONG, clocks.RPMH, clocks.GCC, clocks.UFS, clocks.PCIE, clocks.HS, clocks.SS):
            dest = self.sysfs / ('firmware/devicetree/base' + path)
            dest.mkdir(parents=True, exist_ok=True)
            for key, value in tree[path].items():
                (dest / key).write_bytes(value)
        device = self.sysfs / 'bus/platform/devices/f204008.clock-controller'
        device.mkdir(parents=True)
        (device / 'of_node').symlink_to(self.sysfs / ('firmware/devicetree/base' + clocks.REAL))
        driver = self.sysfs / 'bus/platform/drivers/tcsr_cc-sm8750'
        driver.mkdir(parents=True)
        (device / 'driver').symlink_to(driver)

    def test_normal_modprobe_then_actual_fresh_consumer_proofs_no_marker(self):
        self.fixture()
        calls = []
        def load(argv, check):
            calls.append(argv)
            return SimpleNamespace(returncode=0)
        result = hardware.prepare(self.module, load, self.sysfs)
        self.assertEqual(calls, [['/usr/sbin/modprobe', 'arm_smmu']])
        self.assertEqual([p['scope'] for p in result['proofs']], ['clock', 'usb', 'qup'])
        self.assertFalse(result['full_hardware_ready'])
        self.assertFalse(result['ufs_module_loaded'])
        hardware.publish(result, self.run)
        self.assertTrue((self.run / 'piano-hardware/bootstrap.json').is_file())
        self.assertFalse((self.run / 'piano-smmu-ready').exists())
        (self.run / 'piano-smmu-ready').write_text('ready')
        route = self.sysfs / 'bus/platform/devices/a00000.qcom,gpi-dma/piano_dma_route'
        route.write_text(route.read_text().replace('fsr=00000400', 'fsr=00000402'))
        with self.assertRaises(ValueError):
            hardware.prove(self.module, 'touch', self.sysfs)

    def test_all_configuration_requires_real_context_consumers_not_six_master_success(self):
        self.fixture()
        with self.assertRaises(OSError):
            hardware.prove(self.module, 'all', self.sysfs)
        # Real binding proves clocks belong to normal drivers, not their rate.
        proof = hardware.prove(self.module, 'clock', self.sysfs)
        self.assertFalse(proof['clock_bindings']['clock_enable_or_rate_readback_verified'])
        self.assertEqual(len(proof['clock_bindings']['phy_consumers']), 4)

    def test_wrong_provider_dummy_consumer_or_unbound_driver_refused(self):
        self.fixture()
        node = self.sysfs / 'firmware/devicetree/base/soc/phy@1c06000/clocks'
        original = node.read_bytes()
        node.write_bytes(original[:16] + struct.pack('>I', 0x8ea) + original[20:])
        with self.assertRaises(hardware.Refused):
            hardware.require_clock(self.sysfs)
        node.write_bytes(original)
        driver = self.sysfs / 'bus/platform/devices/f204008.clock-controller/driver'
        driver.unlink()
        with self.assertRaises(OSError):
            hardware.require_clock(self.sysfs)
        calls = []
        with self.assertRaises(OSError):
            hardware.prepare(self.module, lambda *a, **k: calls.append(a), self.sysfs)
        self.assertFalse(calls)

    def test_actual_public_adapters_keep_all_module_commands_and_fresh_guards(self):
        source = ROOT / 'upstream/debian-piano-current'
        result = stage.build(self.path / 'adapters', source)
        self.assertEqual(len(result['source_scripts']), 8)
        for name in stage.PINS:
            before = (source / 'rootfs/overlay/usr/lib/piano' / name).read_text()
            after = (self.path / 'adapters/usr/lib/piano' / name).read_text()
            for line in before.splitlines():
                if line.strip().startswith('modprobe '):
                    # Failure propagation is strengthened for old silent oneshots.
                    command = line.split(' || ', 1)[0]
                    self.assertIn(command, after)
            self.assertNotIn('devmem', after)
            self.assertNotIn('piano-smmu-ready', after)
            self.assertIn('piano-ram-hardware-prepare --require-scope', after)
            subprocess.run(['/bin/sh', '-n', str(self.path / 'adapters/usr/lib/piano' / name)], check=True)
        for name in ('video-start', 'camera-start'):
            text = (self.path / 'adapters/usr/lib/piano' / name).read_text()
            self.assertLess(text.index('echo DMA'), text.index('--require-scope'))
            self.assertLess(text.index('--require-scope'), text.index('\nmodprobe '))
        display = (self.path / 'adapters/usr/lib/piano/display-start').read_text()
        self.assertLess(display.index('modprobe msm $MSM_OPTIONS'), display.index('--require-scope gpu'))
        self.assertLess(display.index('wait_bound adreno'), display.index('--require-scope gpu'))
        self.assertNotIn('--require-scope display\n', display)
        self.assertLess(display.index('--observe-scope mdss-prebind'), display.index('modprobe dispcc_sm8750'))
        self.assertLess(display.index('wait_bound msm_dpu'), display.index('--require-scope display-active'))
        for name in ('adsp-start', 'audio-start'):
            text = (self.path / 'adapters/usr/lib/piano' / name).read_text()
            self.assertIn('disabled by /etc/piano/' + name.split('-')[0] + '.conf"; exit 0;', text)
        with self.assertRaises(ValueError):
            stage.build(self.path / 'adapters', source)
        with self.assertRaises(ValueError):
            stage.adapt('radio-start', b'changed source')

    def test_stage_preflight_rejects_guest_symlink_before_mutation(self):
        dest = ROOT / 'build/distros' / self.path.name
        dest.mkdir()
        try:
            (dest / 'usr').symlink_to(self.path)
            with self.assertRaises(ValueError):
                stage.build(self.path / 'no-write', ROOT / 'upstream/debian-piano-current', dest)
            self.assertFalse((self.path / 'no-write').exists())
        finally:
            (dest / 'usr').unlink()
            dest.rmdir()

    def test_actual_staging_changes_only_adapters_preserves_units_and_enabled_configs(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/distros', prefix='hardware-stage-test-') as temporary:
            dest = Path(temporary) / 'rootfs'
            dest.mkdir()
            source = ROOT / 'upstream/debian-piano-current'
            full_stage.stage(dest, source)
            units = {p.relative_to(dest): p.read_bytes() for p in (dest / 'usr/lib/systemd/system').glob('piano-*.service')}
            configs = {p.relative_to(dest): p.read_bytes() for p in (dest / 'etc/piano').glob('*.conf')}
            result = stage.build(self.path / 'staged-adapters', source, dest)
            for path, data in {**units, **configs}.items():
                self.assertEqual((dest / path).read_bytes(), data)
            for path, digest in result['files'].items():
                self.assertEqual(stage.digest((dest / path).read_bytes()), digest)
            self.assertEqual((dest / 'usr/lib/piano/piano_dma_routes.py').read_bytes(),
                             (ROOT / 'tools/check_piano_kernel_dma_routes.py').read_bytes())
            self.assertFalse(result['device_tested'])
            (dest / 'usr/lib/systemd/system/piano-camera.service').unlink()
            with self.assertRaises(ValueError):
                stage.build(self.path / 'must-not-stage', source, dest)
            self.assertFalse((self.path / 'must-not-stage').exists())

    def context_fixture(self):
        module = hardware.context_checker(ROOT / 'tools/check_piano_kernel_contexts.py')
        actual = read_fdt((ROOT / 'private/analysis/piano-linux-managed-dsp-pcie-v1/Piano-full-linux-managed-dsp-pcie.dtb').read_bytes())['tree']
        for path in ('/soc/iommu@15000000', '/soc/iommu@3da0000'):
            node = self.sysfs / ('firmware/devicetree/base' + path)
            node.mkdir(parents=True, exist_ok=True)
            for key, value in actual[path].items():
                (node / key).write_bytes(value)
        for scope, (device, path, mode, pairs) in module.SCOPES.items():
            node = self.sysfs / ('firmware/devicetree/base' + path)
            node.mkdir(parents=True, exist_ok=True)
            if 'iommus' in actual[path]:
                (node / 'iommus').write_bytes(actual[path]['iommus'])
            else:
                for key, value in actual[path].items():
                    (node / key).write_bytes(value)
            bus = 'pci' if scope == 'radio' else 'platform'
            dev = self.sysfs / ('bus/' + bus + '/devices') / (device or 'created-child-' + scope)
            dev.mkdir(parents=True, exist_ok=True)
            if not (dev / 'of_node').exists():
                (dev / 'of_node').symlink_to(node)
            if device is None:
                driver = self.sysfs / 'bus/platform/drivers' / ('q6apm-dai' if scope == 'audio' else 'qcom,fastrpc-cb')
                driver.mkdir(parents=True, exist_ok=True)
                (dev / 'driver').symlink_to(driver)
            if scope == 'radio':
                (dev / 'vendor').write_text('0x17cb\n')
                (dev / 'device').write_text('0x110e\n')
                host = self.sysfs / 'firmware/devicetree/base/soc/pcie@1c00000'
                host.mkdir(parents=True, exist_ok=True)
                for key, value in actual['/soc/pcie@1c00000'].items():
                    (host / key).write_bytes(value)
            stage1 = mode in ('stage1', 'managed')
            mode = 'stage1' if stage1 else 'identity'
            text = f'piano-dma-{"context" if stage1 else "route"}-v1 domain={mode} ids={len(pairs)}\n'
            for i, (sid, mask) in enumerate(pairs):
                text += (f'sid={sid:x} mask={mask:x} slot={i + 2} origin=kernel-installed '
                         f'smr={0x80000000 | mask << 16 | sid:08x} '
                         f's2cr=00000003 expected=00000003 cb=3 sctlr={1 if stage1 else 0:08x} '
                         'cbar=00010000 fsr=00000400')
                if stage1:
                    text += (' ttbr0=00000000c0000000 ttbr1=00000000c0100000 '
                             'tcr=00802519 tcr2=00038001 mair0=000000ff mair1=00000000')
                text += '\n'
            (dev / ('piano_dma_context' if stage1 else 'piano_dma_route')).write_text(text)
        return module

    def test_actual_shared_context_parser_accepts_initialized_domains_never_dma_transfer(self):
        self.fixture()
        contexts = self.context_fixture()
        for scope in ('video', 'camera', 'gpu', 'gmu', 'mdss', 'adsp', 'audio', 'radio', 'all'):
            with self.subTest(scope=scope):
                proof = hardware.prove(self.module, scope, self.sysfs, contexts)
                self.assertFalse(proof['dma_transfer_verified'])
                self.assertFalse(proof['full_hardware_ready'])
                if scope != 'all':
                    self.assertTrue(all(row['configuration_readback_verified']
                        for context in proof['contexts'] for row in context['routes']))
        video = self.sysfs / 'bus/platform/devices/aa00000.video-codec-ml/piano_dma_context'
        original = video.read_text()
        video.write_text(original.replace('domain=stage1', 'domain=identity'))
        with self.assertRaises(ValueError):
            hardware.prove(self.module, 'video', self.sysfs, contexts)
        video.write_text(original.replace('fsr=00000400', 'fsr=00000402'))
        with self.assertRaises(ValueError):
            hardware.prove(self.module, 'video', self.sysfs, contexts)

    def test_gpu_scope_requires_gmu_and_both_iris_streams_no_optional_fallback(self):
        self.fixture()
        contexts = self.context_fixture()
        gmu = self.sysfs / 'bus/platform/devices/3d6c000.gmu/piano_dma_context'
        gmu.unlink()
        with self.assertRaises(OSError):
            hardware.prove(self.module, 'gpu', self.sysfs, contexts)
        video = self.sysfs / 'bus/platform/devices/aa00000.video-codec-ml/piano_dma_context'
        text = video.read_text()
        video.write_text('\n'.join(text.splitlines()[:2]) + '\n')
        with self.assertRaises(ValueError):
            hardware.prove(self.module, 'video', self.sysfs, contexts)

    def test_actual_main_prebind_live_translation_is_diagnostic_not_probe_blocker(self):
        self.fixture()
        contexts = self.context_fixture()
        path = self.sysfs / 'bus/platform/devices/ae00000.display-subsystem/piano_dma_route'
        # Live firmware can still translate while the normal DPU is unbound.
        path.write_text(path.read_text().replace('sctlr=00000000', 'sctlr=00000001'))
        real_prove = hardware.prove
        real_publish = hardware.publish
        def execute(module, scope):
            return real_prove(module, scope, self.sysfs, contexts)
        for flag, expected in (('--observe-scope', 0), ('--require-scope', 1)):
            with mock.patch.object(hardware, 'require_ram'), \
                 mock.patch.object(hardware, 'checker', return_value=self.module), \
                 mock.patch.object(hardware, 'prove', side_effect=execute), \
                 mock.patch.object(hardware, 'publish', side_effect=lambda report: real_publish(report, self.run)), \
                 mock.patch.object(sys, 'argv', ['piano-ram-hardware-prepare', flag, 'mdss-prebind']), \
                 mock.patch('sys.stderr', new_callable=io.StringIO) as error:
                self.assertEqual(hardware.main(), expected)
                result = json.loads(error.getvalue())
                self.assertEqual(result['status'], 'REFUSED')
                self.assertFalse(result['full_hardware_ready'])
                self.assertEqual(result['observation_only'], flag == '--observe-scope')

    def test_actual_clock_overlay_only_exact_seven_properties_and_rejects_drift(self):
        base = ROOT / 'private/analysis/piano-linux-owned-dma/Piano-full-linux-owned-dma.dtb'
        if not base.exists():
            self.skipTest('actual complete merged DTB required')
        # Tool's output policy requires the explicit private directory.
        with tempfile.TemporaryDirectory(dir=ROOT / 'private/analysis', prefix='clock-test-') as tmp:
            out = Path(tmp) / 'candidate'
            args = SimpleNamespace(base=base, base_sha256=clocks.sha(base.read_bytes()),
                kernel_tree=ROOT / 'build/kernel-worktrees/piano-smmu-routes',
                rom=ROOT / 'private/analysis/android-memory-2026-10-05/live.dtb', output_dir=out,
                dtc=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc',
                fdtoverlay=ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/fdtoverlay',
                libfdt=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
            report = clocks.fold(args)
            self.assertEqual(len(report['changes']), 7)
            self.assertFalse(report['hardware_verified'])
            before = read_fdt(base.read_bytes())
            after = read_fdt((out / 'Piano-full-linux-managed-clocks.dtb').read_bytes())
            self.assertEqual(before['reservations'], after['reservations'])
            self.assertEqual(before['phandles'], after['phandles'])
            for path in before['tree']:
                for key in ('iommus', 'status', 'reg', 'gpios'):
                    self.assertEqual(before['tree'][path].get(key), after['tree'][path].get(key))
            bad = copy.deepcopy(after)
            bad['tree'][clocks.UFS]['clocks'] = before['tree'][clocks.UFS]['clocks']
            with self.assertRaises(ValueError):
                clocks.expected(bad, True)
            args.base_sha256 = '0' * 64
            with self.assertRaises(ValueError):
                clocks.fold(args)


if __name__ == '__main__':
    unittest.main()
