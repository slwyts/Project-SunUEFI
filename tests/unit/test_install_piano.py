"""Synthetic GPT/image fixtures and recorded command refusal; no devices."""
import copy
import base64
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import install_piano as install


def entry(name, first, last, identity, kind=install.LINUX_TYPE, attributes=0):
    value = bytearray(128);value[:16] = kind;value[16:32] = uuid.UUID(identity).bytes_le
    struct.pack_into('<QQQ', value, 32, first, last, attributes)
    encoded = name.encode('utf-16-le');value[56:56 + len(encoded)] = encoded
    return bytes(value)


def fixture_gpt(gib=128, fill=False):
    blocks = gib * 1024**3 // install.BLOCK;last = blocks - 1;count = 128
    table = bytearray(count * 128)
    table[128:256] = entry('userdata', 4096, last - 1040, str(uuid.UUID(int=2)), attributes=0x4000000000000001)
    table[7 * 128:8 * 128] = entry('keepme', 512, 1023, str(uuid.UUID(int=8)), attributes=0x1234)
    if fill:
        for index in range(count):
            if index in (1, 7):continue
            first = 1100 + index * 8
            table[index * 128:(index + 1) * 128] = entry('other' + str(index), first, first + 3, str(uuid.UUID(int=index + 100)))
    def header(here, other, table_lba):
        raw = bytearray(4096);raw[:8] = b'EFI PART'
        struct.pack_into('<IIII', raw, 8, 0x10000, 92, 0, 0)
        struct.pack_into('<QQQQ', raw, 24, here, other, 256, last - 4 - 1)
        raw[56:72] = uuid.UUID(int=1).bytes_le
        struct.pack_into('<QIII', raw, 72, table_lba, count, 128, zlib.crc32(table))
        struct.pack_into('<I', raw, 16, zlib.crc32(raw[:92]))
        return bytes(raw)
    return install.parse_gpt(header(1, last, 2), bytes(table), header(last, 1, last - 4), bytes(table), blocks * 4096)


def superblocks(count, per_section=1):
    raw = bytearray(8192)
    for offset in (1024, 5120):
        struct.pack_into('<IHH', raw, offset, 0xf2f52010, 1, 0)
        struct.pack_into('<IIIII', raw, offset + 8, 9, 3, 12, 9, per_section)
        struct.pack_into('<Q', raw, offset + 36, count)
    return bytes(raw)


def fresh_plan(gpt):
    userdata = next(row for row in gpt.partitions if row['name'] == 'userdata')
    return install.plan_gpt(gpt, install.f2fs_superblocks(superblocks(userdata['bytes'] // 4096 - 5)),
                            [str(uuid.UUID(int=1000)), str(uuid.UUID(int=1001))])


def update_snapshot():
    original = fixture_gpt();plan = fresh_plan(original)
    writes = install.fresh_write_schedule(original, plan, True, superblocks(plan['new_userdata_blocks'] - 5))
    _, table = writes[0];_, backup = writes[1];_, primary = writes[-1]
    current = install.parse_gpt(primary, table, backup, table, original.disk_bytes)
    return {'identity': {'product': 'piano', 'unlocked': True, 'factory_serial': 'SYNTHETIC001',
                         'disk': '/dev/block/sdz', 'disk_bytes': current.disk_bytes, 'lun': 0, 'block_bytes': 4096},
            'gpt': current, 'f2fs': install.f2fs_superblocks(superblocks(plan['new_userdata_blocks'] - 5))}


def fixture_bundle(directory, raw_root=True):
    root = Path(directory);root.mkdir(exist_ok=True)
    esp = bytearray(65536);esp[510:512] = b'\x55\xaa';esp[82:90] = b'FAT32   ';esp[71:82] = b'SUNUEFI_ESP'
    struct.pack_into('<H', esp, 11, 512);struct.pack_into('<I', esp, 32, len(esp) // 512)
    (root / 'esp.img').write_bytes(esp)
    files = {'esp.img': {'bytes': len(esp), 'sha256': install.sha(esp), 'format': 'raw-fat32', 'expanded_bytes': len(esp)}}
    if raw_root:
        ext4 = bytearray(65536);ext4[1080:1082] = b'\x53\xef'
        struct.pack_into('<I', ext4, 1028, 16);struct.pack_into('<I', ext4, 1048, 2)
        ext4[1144:1153] = b'PIANOROOT';(root / 'root.ext4.img').write_bytes(ext4)
        files['root.ext4.img'] = {'bytes': len(ext4), 'sha256': install.sha(ext4), 'format': 'raw-ext4', 'expanded_bytes': len(ext4)}
    else:
        (root / 'root.tar.zst').write_bytes(b'fixture archive')
        files['root.tar.zst'] = {'bytes': 15, 'sha256': install.file_sha(root / 'root.tar.zst'), 'format': 'tar-zstd'}
    manifest = {'schema_version': 1, 'status': 'HOST_BUILT_NOT_DEVICE_READY', 'root_policy': 'LABEL=PIANOROOT', 'files': files}
    (root / 'manifest.json').write_text(json.dumps(manifest))
    return root


class FakeDevice:
    def __init__(self, snapshot, hashes):
        self.snapshot, self.hashes, self.events = snapshot, hashes, []

    def inspect(self):
        self.events.append('inspect');return self.snapshot

    def reboot_bootloader(self):
        self.events.append('bootloader')

    def flash(self, identity, partition, image):
        self.events.append(('flash', partition['name']))

    def reboot_android(self, identity):
        self.events.append('android')

    def readback_hash(self, name, size):
        self.events.append(('readback', name));return self.hashes[name]


class InstallerTests(unittest.TestCase):
    def test_ssh_plan_binds_key_and_refuses_missing_or_changed_key_before_flash(self):
        snapshot = update_snapshot()
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory)
            key = Path(directory) / 'access.pub'
            other = Path(directory) / 'other.pub'
            for path, hex_key in ((key, 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'),
                                  (other, '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c')):
                wire = struct.pack('>I', 11) + b'ssh-ed25519' + struct.pack('>I', 32) + bytes.fromhex(hex_key)
                path.write_bytes(b'ssh-ed25519 ' + base64.b64encode(wire) + b'\n')
            plan = install.make_plan(snapshot, bundle, ssh_public_key=key)
            self.assertEqual(plan['ssh_provisioning']['source_root_sha256'],
                             plan['bundle']['images']['root.ext4.img']['sha256'])
            fake = FakeDevice(snapshot, {})
            for supplied in (None, other):
                fake.events.clear()
                with self.assertRaisesRegex(ValueError, 'changed since plan'):
                    install.apply_update(fake, plan, bundle, execute=True, ssh_public_key=supplied)
                self.assertEqual(fake.events, ['inspect'])
            fake.events.clear()
            preview = install.apply_update(fake, plan, bundle, ssh_public_key=key)
            self.assertEqual(preview['ssh_provisioning_pending'], plan['ssh_provisioning'])
            self.assertFalse(preview['device_writes'])
            self.assertEqual(fake.events, ['inspect'])

    def test_fresh_exact_64g_alignment_and_unchanged_other_entries(self):
        original = fixture_gpt();plan = fresh_plan(original)
        self.assertEqual(sum(row['bytes'] for row in plan['partitions']), 64 * 1024**3)
        self.assertEqual(plan['partitions'][0]['bytes'], 512 * 1024**2)
        self.assertTrue(all(row['first'] % 512 == 0 and (row['last'] + 1) % 512 == 0 for row in plan['partitions']))
        writes = install.fresh_write_schedule(original, plan, True, superblocks(plan['new_userdata_blocks'] - 5))
        self.assertEqual([lba for lba, _ in writes], [original.backup_table_lba, original.disk_bytes // 4096 - 1, original.geometry['table_lba'], 1])
        table = writes[0][1]
        changed = {row['index'] for row in plan['partitions']} | {plan['userdata']['index']}
        for index in range(original.geometry['count']):
            if index not in changed:
                self.assertEqual(table[index * 128:(index + 1) * 128], original.primary_entries[index * 128:(index + 1) * 128])
        old = bytearray(original.primary_entries[128:256]);struct.pack_into('<Q', old, 40, plan['new_userdata_last'])
        self.assertEqual(bytes(old), table[128:256])

    def test_crc_table_disagreement_and_overlap_refusals(self):
        original = fixture_gpt()
        header = bytearray(original.primary);header[16] ^= 1
        with self.assertRaisesRegex(ValueError, 'header CRC'):
            install.parse_gpt(bytes(header), original.primary_entries, original.backup, original.backup_entries, original.disk_bytes)
        table = bytearray(original.backup_entries);table[100] ^= 1
        with self.assertRaisesRegex(ValueError, 'tables differ'):
            install.parse_gpt(original.primary, original.primary_entries, original.backup, bytes(table), original.disk_bytes)
        table = bytearray(original.primary_entries)
        struct.pack_into('<QQ', table, 7 * 128 + 32, 4096, 5000)
        crc = zlib.crc32(table)
        with self.assertRaisesRegex(ValueError, 'overlap'):
            install.parse_gpt(install._new_header(original.primary, crc), bytes(table), install._new_header(original.backup, crc), bytes(table), original.disk_bytes)

    def test_space_free_slots_fs_section_and_guid_refusals(self):
        with self.assertRaisesRegex(ValueError, 'Insufficient'):
            fresh_plan(fixture_gpt(40))
        with self.assertRaisesRegex(ValueError, 'empty GPT entries'):
            fresh_plan(fixture_gpt(fill=True))
        original = fixture_gpt()
        with self.assertRaisesRegex(ValueError, '512-block'):
            install.plan_gpt(original, {'block_count': 10, 'section_blocks': 1024})
        with self.assertRaisesRegex(ValueError, 'collide'):
            install.plan_gpt(original, {'block_count': 100, 'section_blocks': 512}, [str(uuid.UUID(int=2)), str(uuid.UUID(int=1000))])

    def test_fresh_guard_never_admits_missing_ioctl_oversized_or_divergent_superblocks(self):
        original = fixture_gpt();plan = fresh_plan(original)
        with self.assertRaisesRegex(ValueError, 'ioctl'):
            install.fresh_write_schedule(original, plan, False, superblocks(plan['new_userdata_blocks'] - 5))
        with self.assertRaisesRegex(ValueError, 'fit'):
            install.fresh_write_schedule(original, plan, True, superblocks(plan['new_userdata_blocks'] + 1))
        different = bytearray(superblocks(plan['new_userdata_blocks'] - 5));different[5120 + 36] ^= 1
        with self.assertRaisesRegex(ValueError, 'disagree'):
            install.fresh_write_schedule(original, plan, True, bytes(different))
        changed = copy.deepcopy(plan);changed['partitions'][0]['first'] += 512
        with self.assertRaisesRegex(ValueError, 'altered'):
            install.fresh_write_schedule(original, changed, True, superblocks(plan['new_userdata_blocks'] - 5))

    def test_fresh_execute_unconditionally_refuses_before_device_or_gpt_operations(self):
        original = fixture_gpt();plan = {'schema_version': 1, 'status': 'READ_ONLY_PLAN', 'gpt': fresh_plan(original)}
        fake = FakeDevice(None, {})
        with self.assertRaisesRegex(ValueError, 'NEW_INSTALL_NOT_READY'):
            install.apply_update(fake, plan, '.', execute=True)
        self.assertEqual(fake.events, [])

    def test_existing_partitions_select_update(self):
        snapshot = update_snapshot();plan = install.plan_gpt(snapshot['gpt'])
        self.assertEqual(plan['mode'], 'update');self.assertFalse(plan['gpt_writes'])
        self.assertEqual([row['name'] for row in plan['partitions']], list(install.PARTITIONS))

    def test_readonly_inspection_can_report_unavailable_plaintext_fs_without_planning(self):
        snapshot = update_snapshot();snapshot['f2fs'] = None
        report = install.inspection_report(snapshot)
        self.assertEqual(report['status'], 'READ_ONLY_INSPECTION')
        self.assertIsNone(report['f2fs']);self.assertFalse(report['device_writes'])

    def test_update_preview_is_read_only_and_execute_only_flashes_two_dedicated_volumes(self):
        snapshot = update_snapshot()
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory)
            plan = install.make_plan(snapshot, bundle)
            hashes = {value['partition']: value['sha256'] for value in plan['bundle']['images'].values()}
            fake = FakeDevice(snapshot, hashes)
            # The normal installer must also work without an Android factory
            # address provider: Linux configures controller identity at boot.
            result = install.apply_installation(fake, plan, bundle)
            self.assertFalse(result['device_writes']);self.assertEqual(fake.events, ['inspect'])
            fake.events.clear();result = install.apply_installation(fake, plan, bundle, execute=True)
            self.assertEqual(result['status'], 'UPDATE_PREFIX_READBACK_VERIFIED')
            self.assertEqual([event for event in fake.events if isinstance(event, tuple) and event[0] == 'flash'],
                             [('flash', 'sunuefi_esp'), ('flash', 'sunuefi_root')])
            self.assertFalse(result['gpt_changed']);self.assertFalse(result['linux_boot_verified'])

    def test_changed_plan_or_bundle_refuses_before_any_write(self):
        snapshot = update_snapshot()
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory);plan = install.make_plan(snapshot, bundle)
            bad = copy.deepcopy(plan);bad['gpt']['baseline']['primary'] = '0' * 64
            fake = FakeDevice(snapshot, {})
            with self.assertRaisesRegex(ValueError, 'changed since plan'):
                install.apply_update(fake, bad, bundle, execute=True)
            self.assertEqual(fake.events, ['inspect'])
            (bundle / 'esp.img').write_bytes(b'\0' * 65536)
            with self.assertRaises(ValueError):
                install.apply_update(fake, plan, bundle, execute=True)
            self.assertNotIn('bootloader', fake.events)

    def test_readback_mismatch_is_not_reported_as_success(self):
        snapshot = update_snapshot()
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory);plan = install.make_plan(snapshot, bundle)
            fake = FakeDevice(snapshot, {name: '0' * 64 for name in install.PARTITIONS})
            with self.assertRaisesRegex(ValueError, 'readback mismatch'):
                install.apply_update(fake, plan, bundle, execute=True)

    def test_tar_only_incomplete_sparse_bad_sha_and_capacity_refusals(self):
        partitions = install.plan_gpt(update_snapshot()['gpt'])['partitions']
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory, raw_root=False)
            self.assertFalse(install.load_bundle(bundle, partitions)['update_ready'])
            with self.assertRaisesRegex(ValueError, 'tar-only'):
                install.load_bundle(bundle, partitions, require_images=True)
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory);(bundle / '.incomplete').write_text('in progress')
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                install.load_bundle(bundle, partitions)
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory)
            raw = bytearray((bundle / 'esp.img').read_bytes());struct.pack_into('<I', raw, 0, 0xed26ff3a)
            (bundle / 'esp.img').write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'sparse'):
                install.load_bundle(bundle, partitions)
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory)
            raw = bytearray((bundle / 'esp.img').read_bytes());raw[-1] ^= 1;(bundle / 'esp.img').write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                install.load_bundle(bundle, partitions)
        with tempfile.TemporaryDirectory() as directory:
            bundle = fixture_bundle(directory);small = copy.deepcopy(partitions);small[0]['bytes'] = 4096
            with self.assertRaisesRegex(ValueError, 'capacity'):
                install.load_bundle(bundle, small)

    def test_factory_bootloader_identity_capacity_and_partition_allowlist(self):
        device = install.Device('SYNTHETIC001', runner=lambda *args, **kwargs: self.fail('No external command expected'))
        partition = install.plan_gpt(update_snapshot()['gpt'])['partitions'][0]
        values = {'serialno': 'SYNTHETIC001', 'product': 'piano', 'unlocked': 'yes', 'is-userspace': 'no',
                  'partition-size:sunuefi_esp': hex(partition['bytes'])}
        device.fastboot_var = lambda serial, key: values[key]
        device.check_fastboot({'factory_serial': 'SYNTHETIC001'}, partition)
        values['serialno'] = 'SunUEFI-piano'
        with self.assertRaisesRegex(ValueError, 'identity'):
            device.check_fastboot({'factory_serial': 'SYNTHETIC001'}, partition)
        with self.assertRaisesRegex(ValueError, 'dedicated'):
            device.flash({}, {'name': 'recovery_a'}, Path('fake.img'))
        with self.assertRaisesRegex(ValueError, 'factory'):
            install.Device('SunUEFI-piano')

    def test_recovery_request_rejected_before_any_device_command(self):
        fake = FakeDevice(None, {})
        with self.assertRaisesRegex(ValueError, 'RECOVERY_INSTALL_NOT_READY'):
            install.apply_update(fake, {}, '.', execute=True, recovery=True)
        self.assertEqual(fake.events, [])

    def test_android_helper_actual_superblock_parser_without_ioctl_or_device(self):
        compiler = shutil.which('cc')
        if compiler is None:self.skipTest('Host C compiler unavailable')
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory);image = tmp / 'fixture.bin';image.write_bytes(superblocks(100000))
            source = tmp / 'check.c'
            source.write_text('#define main helper_main\n#include "' + str(ROOT / 'tools/android/piano_resize_f2fs.c') + '"\n#undef main\n'
                              'int main(int argc,char **argv){uint64_t n=0,s=0;int f=open(argv[1],O_RDONLY);'
                              '(void)argc;if(f<0||supers(f,&n,&s)||n!=100000||s!=512)return 1;close(f);return 0;}\n')
            executable = tmp / 'check'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable), str(image)], check=True)
            raw = bytearray(image.read_bytes());raw[5120 + 36] ^= 1;image.write_bytes(raw)
            result = subprocess.run([str(executable), str(image)], check=False)
            self.assertEqual(result.returncode, 1)


if __name__ == '__main__':
    unittest.main()
