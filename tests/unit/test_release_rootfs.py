"""Release policy and dry planning with disposable trees; no APT/device/root calls."""
import copy
import importlib.util
from importlib.machinery import SourceFileLoader
import json
import os
from pathlib import Path
import subprocess
import stat
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / 'tools'))
SPEC = importlib.util.spec_from_file_location('release_rootfs', PROJECT / 'tools/build_release_rootfs.py')
builder = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(builder)
CONFIG = json.loads((PROJECT / 'config/release.json').read_text())
BOOT = (PROJECT / 'linux/userspace/release-disk-bootstrap').read_text()
LOADER = SourceFileLoader('canonical_label_adapter', str(PROJECT / 'linux/userspace/piano-ram-hardware-prepare'))
ADAPTER_SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
adapter = importlib.util.module_from_spec(ADAPTER_SPEC); LOADER.exec_module(adapter)


class ReleaseRootTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / 'build/distros/release'; self.rootfs = self.output / 'rootfs'
        self.kernel = self.root / 'artifacts/kernels/release'; self.source = self.root / 'build/kernel-worktrees/release-kernel'
        for path in (self.kernel, self.source, self.root / 'config'):
            path.mkdir(parents=True)
        (self.kernel / 'manifest.json').write_text('{}'); (self.source / 'Makefile').write_text('fixture')
        (self.root / 'config/release.json').write_text(json.dumps(CONFIG))
        debug = self.root / 'linux/userspace/piano-debug-bootstrap'
        debug.parent.mkdir(parents=True); debug.write_text('#!/bin/sh\nexit 0\n')
        self.m = {'source_commit': 'a' * 40, 'source_clean': True, 'root_policy': 'LABEL=PIANOROOT',
                  'command_line': 'rdinit=/pianoinit piano.root=LABEL=PIANOROOT', 'kernel_release': 'fixture-release'}

    def planned(self):
        with patch.object(builder.modules, 'inspect', return_value=(self.m, 'b' * 64)), patch.object(builder, 'repository', return_value='a' * 40):
            return builder.plan(self.kernel, self.source, self.output, root=self.root)

    def policy_tree(self):
        for name in ('usr/lib/piano', 'etc/systemd/system', 'etc/ssh', 'home/piano/.ssh', 'root/.ssh'):
            (self.rootfs / name).mkdir(parents=True, exist_ok=True)
        (self.rootfs / 'usr/lib/piano/piano-ram-hardware-prepare').write_text('real adapter fixture')
        for name in ('piano-display.service', 'piano-touch.service', 'piano-keyboard.service', 'piano-audio.service'):
            (self.rootfs / 'etc/systemd/system' / name).write_text('hardware fixture ' + name)

    def test_plan_does_not_create_output_or_execute_commands(self):
        with patch.object(builder.subprocess, 'run', side_effect=AssertionError('plan executed a command')):
            result = self.planned()
        self.assertEqual(result['status'], 'PLAN_ONLY'); self.assertEqual(result['root_policy'], 'LABEL=PIANOROOT')
        self.assertIn('Piano Mesa .deb directory (--mesa-dir)', result['missing_inputs'])
        self.assertTrue(any('build-rootfs.sh' in step for step in result['steps']))
        self.assertFalse(self.output.exists())

    def test_existing_output_and_path_outside_project_are_refused(self):
        self.output.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'new project'):
            self.planned()
        with self.assertRaisesRegex(ValueError, 'new project'):
            builder.plan(self.kernel, self.source, self.root / 'old-root', root=self.root)

    def test_kernel_uuid_userdata_and_wrong_actual_commit_are_refused(self):
        for policy, command in (('PARTUUID=fixture', 'piano.root=PARTUUID=fixture'),
                                ('LABEL=PIANOROOT', 'piano.root=LABEL=PIANOROOT root=PARTLABEL=userdata'),
                                ('LABEL=PIANOROOT', 'piano.root=LABEL=PIANOROOT root=/dev/sda'),
                                ('LABEL=PIANOROOT', 'piano.root=LABEL=PIANOROOT piano.root=LABEL=PIANOROOT')):
            self.m.update(root_policy=policy, command_line=command)
            with self.subTest(policy=policy), self.assertRaisesRegex(ValueError, 'never Android'):
                self.planned()
        self.m.update(root_policy='LABEL=PIANOROOT', command_line='piano.root=LABEL=PIANOROOT', source_commit='c' * 40)
        with self.assertRaisesRegex(ValueError, 'prepared release source'):
            self.planned()

    def test_missing_inputs_cannot_be_reported_as_a_real_build(self):
        record = self.planned()
        with patch.object(builder.subprocess, 'run', side_effect=AssertionError('missing input installed something')):
            with self.assertRaisesRegex(ValueError, 'Missing inputs'):
                builder.execute(record)
        self.assertFalse(self.output.exists())

    def test_nonroot_runner_is_refused_before_host_build(self):
        with patch.object(builder.os, 'geteuid', return_value=1000), self.assertRaisesRegex(ValueError, 'requires root'):
            builder.require_host()

    def test_label_policy_preserves_hardware_and_protects_android(self):
        self.policy_tree()
        before = {p.name: p.read_bytes() for p in (self.rootfs / 'etc/systemd/system').glob('piano-*.service')}
        with patch.object(builder, 'ROOT', self.root):
            builder.apply_policy(self.rootfs, self.output, CONFIG)
            builder.apply_policy(self.rootfs, self.output, CONFIG)
        self.assertEqual((self.rootfs / 'etc/fstab').read_text(), 'LABEL=PIANOROOT / ext4 defaults,noatime 0 1\nLABEL=SUNUEFI_ESP /boot/efi vfat umask=0077,nofail 0 2\n')
        for name, data in before.items(): self.assertEqual((self.rootfs / 'etc/systemd/system' / name).read_bytes(), data)
        for name in CONFIG['storage']['masked_units']:
            self.assertEqual(os.readlink(self.rootfs / 'etc/systemd/system' / name), '/dev/null')
        rules = (self.rootfs / 'etc/udev/rules.d/01-piano-protect-android.rules').read_text()
        root_rule = next(line for line in rules.splitlines() if 'PIANOROOT' in line)
        self.assertIn('ID_PART_ENTRY_NAME}=="sunuefi_root"', root_rule); self.assertIn('ID_FS_TYPE}=="ext4"', root_rule)
        self.assertIn('blockdev --setro', rules); self.assertNotIn('userdata', (self.rootfs / 'etc/fstab').read_text())

    def test_unknown_storage_or_hardware_mask_cannot_change_root(self):
        self.policy_tree()
        for field, value in (('root_label', 'userdata'), ('masked_units', ['piano-touch.service'])):
            config = copy.deepcopy(CONFIG); config['storage'][field] = value
            with patch.object(builder, 'ROOT', self.root), self.assertRaises(ValueError):
                builder.apply_policy(self.rootfs, self.output, config)
        self.assertFalse((self.rootfs / 'etc/fstab').exists())

    def test_sanitize_removes_only_new_generated_identity_and_locks_password(self):
        self.policy_tree(); old = self.root / 'old-root'; old.mkdir(); (old / 'keep').write_text('unchanged')
        for name in ('root/.ssh/authorized_keys', 'home/piano/.ssh/authorized_keys', 'etc/ssh/ssh_host_ed25519_key'):
            (self.rootfs / name).write_text('synthetic fixture')
        for name in ('access-key', 'access-key.pub', 'login.txt'): (self.output / name).write_text('synthetic fixture')
        (self.rootfs / 'etc/shadow-').write_text('synthetic old password backup')
        with patch.object(builder, 'ROOT', self.root), patch.object(builder.subprocess, 'run') as run:
            builder.sanitize(self.rootfs, self.output)
        self.assertEqual((self.rootfs / 'etc/machine-id').read_bytes(), b'')
        self.assertFalse(list((self.rootfs / 'etc/ssh').glob('ssh_host_*')))
        self.assertFalse((self.output / 'login.txt').exists()); self.assertEqual((old / 'keep').read_text(), 'unchanged')
        self.assertFalse((self.rootfs / 'etc/shadow-').exists())
        self.assertEqual(run.call_args.args[0][-3:], ['--password', '!', 'piano'])
        with patch.object(builder, 'ROOT', self.root), self.assertRaises(ValueError): builder.sanitize(old, old.parent)

    def test_guest_symlink_cannot_redirect_policy_to_an_old_tree(self):
        self.policy_tree(); old = self.root / 'old-root'; old.mkdir()
        (self.rootfs / 'etc/piano').symlink_to(old)
        with patch.object(builder, 'ROOT', self.root), self.assertRaisesRegex(ValueError, 'symlink'):
            builder.apply_policy(self.rootfs, self.output, CONFIG)
        self.assertFalse((old / 'root-policy.json').exists())

    def test_snapshot_claim_without_implemented_snapshot_runner_is_rejected(self):
        config = copy.deepcopy(CONFIG); config['debian']['apt_policy'] = 'snapshot'; config['debian']['snapshot'] = 'unverified'
        (self.root / 'config/release.json').write_text(json.dumps(config))
        with self.assertRaisesRegex(ValueError, 'snapshot mode'):
            self.planned()

    def test_real_shell_selector_rejects_android_uuid_and_duplicates(self):
        guard = BOOT[BOOT.index('selected=0'):BOOT.index('dma_routes() {')]
        harness = 'expected=PIANOROOT\nfail() { exit 7; }\ncat() { printf "%s\\n" "$TEST_CMDLINE"; }\n'
        for command, code in (('piano.root=LABEL=PIANOROOT', 0), ('piano.root=PARTUUID=old', 7),
                              ('piano.root=LABEL=PIANOROOT root=PARTLABEL=userdata', 7),
                              ('piano.root=LABEL=PIANOROOT piano.root=LABEL=PIANOROOT', 7)):
            result = subprocess.run(['sh', '-c', harness + guard], env={**os.environ, 'TEST_CMDLINE': command}, capture_output=True)
            self.assertEqual(result.returncode, code, command)
        self.assertIn('PARTNAME=sunuefi_root', BOOT)
        self.assertLess(BOOT.index("verify_device || fail 'LABEL/PARTNAME/TYPE mismatch'"), BOOT.index('mount -t ext4'))

    def canonical_label(self, answers):
        proc, run = self.root / 'proc-fixture', self.root / 'run-fixture'
        proc.mkdir(); run.mkdir()
        (proc / 'cmdline').write_text('piano.root=LABEL=PIANOROOT')
        (proc / 'mounts').write_text('/dev/sda4 / ext4 rw 0 0\n')
        (run / 'piano-root-mode').write_text('LABEL=PIANOROOT'); (run / 'piano-root-device').write_text('/dev/sda4')
        real_read, real_stat, real_dir = Path.read_text, os.stat, Path.is_dir
        def read(path, *args, **kwargs):
            if str(path) == '/etc/piano/root-policy.json':
                return json.dumps({'mode': 'label', 'root_label': 'PIANOROOT', 'root_partlabel': 'sunuefi_root'})
            return real_read(path, *args, **kwargs)
        def info(path, *args, **kwargs):
            if str(path) == '/dev/sda4': return SimpleNamespace(st_mode=stat.S_IFBLK | 0o600, st_rdev=7)
            if str(path) == '/': return SimpleNamespace(st_dev=7)
            return real_stat(path, *args, **kwargs)
        def directory(path):
            return True if str(path) == '/usr/lib/modules/fixture-release' else real_dir(path)
        with patch.object(Path, 'read_text', read), patch.object(Path, 'is_dir', directory), \
             patch.object(adapter.os, 'stat', info), patch.object(adapter.os, 'uname', return_value=SimpleNamespace(release='fixture-release')), \
             patch.object(adapter.subprocess, 'check_output', side_effect=answers) as blkid:
            adapter.require_ram(proc, run)
        self.assertEqual(blkid.call_count, 2)
        self.assertEqual(blkid.call_args.args[0], ['/usr/sbin/blkid', '-p', '-o', 'export', '/dev/sda4'])

    def test_canonical_adapter_accepts_stable_label_partname_and_ext4(self):
        value = 'LABEL=PIANOROOT\nPART_ENTRY_NAME=sunuefi_root\nTYPE=ext4\n'
        self.canonical_label([value, value])

    def test_canonical_adapter_rejects_userdata_even_with_matching_label(self):
        value = 'LABEL=PIANOROOT\nPART_ENTRY_NAME=userdata\nTYPE=ext4\n'
        with self.assertRaisesRegex(adapter.Refused, 'partition name'):
            self.canonical_label([value, value])

    def test_canonical_adapter_rejects_identity_readback_drift(self):
        value = 'LABEL=PIANOROOT\nPART_ENTRY_NAME=sunuefi_root\nTYPE=ext4\n'
        with self.assertRaisesRegex(adapter.Refused, 'changed during inspection'):
            self.canonical_label([value, value.replace('PIANOROOT', 'OTHER')])


if __name__ == '__main__': unittest.main()
