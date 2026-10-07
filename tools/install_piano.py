#!/usr/bin/env python3
"""Read-only Piano plans and explicit updates of existing dedicated OS volumes.

Fresh partition execution is deliberately unavailable until the Android shrink
helper and device transport have been compiled and validated. No GPT writer or
stock boot/recovery/system/vbmeta flashing is implemented here.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import struct
import subprocess
import uuid
import zlib

BLOCK = 4096
ALIGN = 512  # One supported F2FS section: 512 * 4 KiB.
TOTAL = 64 * 1024**3
ESP_BYTES = 512 * 1024**2
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
LINUX_TYPE = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
BASIC_TYPE = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
PARTITIONS = ('sunuefi_esp', 'sunuefi_root')
IMAGE_NAMES = ('esp.img', 'root.ext4.img')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _json_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    path = Path(path)
    require(not path.is_symlink() and path.is_file() and path.stat().st_size <= 1024 * 1024,
            'Missing/oversized/symlink JSON input')
    return json.loads(path.read_text(), object_pairs_hook=_json_pairs)


def gpt_header(data, here, last):
    require(len(data) == BLOCK and data[:8] == b'EFI PART', 'Expected a complete 4 KiB GPT header')
    revision, size, crc, reserved = struct.unpack_from('<IIII', data, 8)
    current, alternate, first, final = struct.unpack_from('<QQQQ', data, 24)
    table, count, width, array_crc = struct.unpack_from('<QIII', data, 72)
    require(revision == 0x10000 and 92 <= size <= BLOCK and reserved == 0, 'GPT revision/header size invalid')
    clean = bytearray(data[:size]);struct.pack_into('<I', clean, 16, 0)
    require(zlib.crc32(clean) == crc, 'GPT header CRC mismatch')
    require(current == here and alternate == (last if here == 1 else 1), 'GPT header locations differ')
    require(2 <= first <= final < last and 1 <= count <= 4096 and width == 128, 'Unsupported GPT usable area/entries')
    blocks = (count * width + BLOCK - 1) // BLOCK
    require((here == 1 and 2 <= table and table + blocks <= first) or
            (here == last and final < table and table + blocks <= last), 'GPT table overlaps usable area')
    return {'first': first, 'last': final, 'table_lba': table, 'count': count,
            'width': width, 'array_crc': array_crc, 'table_blocks': blocks,
            'disk_guid': str(uuid.UUID(bytes_le=data[56:72]))}


@dataclass(frozen=True)
class Gpt:
    disk_bytes: int
    primary: bytes
    primary_entries: bytes
    backup: bytes
    backup_entries: bytes
    geometry: dict
    backup_table_lba: int
    partitions: tuple

    def baseline(self):
        return {name: sha(getattr(self, name)) for name in
                ('primary', 'primary_entries', 'backup', 'backup_entries')}


def parse_gpt(primary, primary_entries, backup, backup_entries, disk_bytes):
    require(type(disk_bytes) is int and disk_bytes % BLOCK == 0 and disk_bytes >= 4 * BLOCK, 'Disk must use 4 KiB blocks')
    last = disk_bytes // BLOCK - 1
    front, rear = gpt_header(primary, 1, last), gpt_header(backup, last, last)
    for key in ('first', 'last', 'count', 'width', 'array_crc', 'disk_guid'):
        require(front[key] == rear[key], 'Primary/backup GPT metadata differ: ' + key)
    require(len(primary_entries) == len(backup_entries) == front['table_blocks'] * BLOCK and
            primary_entries == backup_entries, 'Primary/backup GPT tables differ')
    used = primary_entries[:front['count'] * front['width']]
    require(zlib.crc32(used) == front['array_crc'], 'GPT entry array CRC mismatch')
    partitions, guids, names = [], set(), set()
    for index in range(front['count']):
        raw = used[index * 128:(index + 1) * 128]
        if not any(raw[:16]):
            continue
        first, end, attributes = struct.unpack_from('<QQQ', raw, 32)
        name = raw[56:128].decode('utf-16-le').rstrip('\0')
        require('\0' not in name and front['first'] <= first <= end <= front['last'], 'Invalid GPT partition name/range')
        identity = str(uuid.UUID(bytes_le=raw[16:32]))
        require(any(raw[16:32]) and identity not in guids and (not name or name not in names), 'Duplicate/empty GPT identity')
        guids.add(identity);names.add(name)
        partitions.append({'index': index, 'name': name, 'guid': identity,
                           'type': str(uuid.UUID(bytes_le=raw[:16])), 'first': first,
                           'last': end, 'bytes': (end - first + 1) * BLOCK, 'attributes': attributes})
    ordered = sorted(partitions, key=lambda row: row['first'])
    require(all(before['last'] < after['first'] for before, after in zip(ordered, ordered[1:])), 'GPT partitions overlap')
    require(sum(row['name'] == 'userdata' for row in partitions) == 1, 'Expected exactly one userdata partition')
    return Gpt(disk_bytes, primary, primary_entries, backup, backup_entries, front, rear['table_lba'], tuple(partitions))


def f2fs_superblocks(data):
    require(len(data) >= 2 * BLOCK, 'Both plaintext F2FS superblocks are required')
    require(data[1024:1088] == data[BLOCK + 1024:BLOCK + 1088], 'F2FS superblocks disagree')
    values = []
    for offset in (1024, BLOCK + 1024):
        magic = struct.unpack_from('<I', data, offset)[0]
        log_sector, log_per_block, log_block, log_segment, per_section = struct.unpack_from('<IIIII', data, offset + 8)
        count = struct.unpack_from('<Q', data, offset + 36)[0]
        require(magic == 0xf2f52010 and log_block == 12 and log_sector in (9, 10, 11, 12) and log_sector + log_per_block == 12 and
                log_segment == 9 and per_section > 0 and count > 0, 'Unsupported/plaintext F2FS superblock')
        values.append({'block_count': count, 'section_blocks': (1 << log_segment) * per_section})
    require(values[0] == values[1], 'F2FS superblocks disagree')
    return values[0]


def plan_gpt(gpt, f2fs=None, new_guids=None):
    by_name = {row['name']: row for row in gpt.partitions}
    present = [name in by_name for name in PARTITIONS]
    require(present[0] == present[1], 'Only one dedicated OS partition exists; manual review required')
    base = {'baseline': gpt.baseline(), 'disk_bytes': gpt.disk_bytes, 'block_bytes': BLOCK,
            'userdata': dict(by_name['userdata'])}
    if all(present):
        esp, root = (by_name[name] for name in PARTITIONS)
        require(uuid.UUID(esp['type']).bytes_le == ESP_TYPE and
                uuid.UUID(root['type']).bytes_le in (LINUX_TYPE, BASIC_TYPE), 'Dedicated OS partition types differ')
        require(esp['bytes'] >= ESP_BYTES and root['bytes'] > 0, 'Dedicated OS partition capacity invalid')
        return {**base, 'mode': 'update', 'partitions': [dict(esp), dict(root)],
                'gpt_writes': False, 'execution_ready': False,
                'execution_requirements': ['matching reviewed plan', 'verified raw bundle',
                                           'factory fastboot identity/capacity', 'device prefix readback']}
    require(f2fs is not None and f2fs.get('section_blocks') == ALIGN, 'Fresh plan requires matching 512-block F2FS sections')
    userdata = by_name['userdata']
    require(userdata['first'] % ALIGN == 0 and 0 < f2fs['block_count'] <= userdata['bytes'] // BLOCK,
            'Userdata alignment/F2FS capacity differs')
    start = ((userdata['last'] + 1 - TOTAL // BLOCK) // ALIGN) * ALIGN
    require(start > userdata['first'] + ALIGN and (start - userdata['first']) * BLOCK >= 8 * 1024**3,
            'Insufficient userdata space for 64 GiB and a retained filesystem')
    empty = [index for index in range(gpt.geometry['count'])
             if not any(gpt.primary_entries[index * 128:(index + 1) * 128])]
    require(len(empty) >= 2, 'Two completely empty GPT entries are required')
    identities = [str(uuid.UUID(value)) for value in new_guids] if new_guids is not None else [str(uuid.uuid4()), str(uuid.uuid4())]
    existing = {row['guid'] for row in gpt.partitions}
    require(len(identities) == 2 and len(set(identities)) == 2 and not set(identities) & existing and
            all(uuid.UUID(value).int for value in identities), 'New partition GUIDs collide/are empty')
    parts = []
    for name, size, kind, index, identity in zip(PARTITIONS, (ESP_BYTES, TOTAL - ESP_BYTES),
                                              (ESP_TYPE, LINUX_TYPE), empty[:2], identities):
        end = start + size // BLOCK - 1
        parts.append({'index': index, 'name': name, 'guid': identity,
                      'type': str(uuid.UUID(bytes_le=kind)), 'first': start,
                      'last': end, 'bytes': size, 'attributes': 0})
        start = end + 1
    new_end = parts[0]['first'] - 1
    require(parts[-1]['last'] <= userdata['last'], 'Fresh partitions exceed original userdata')
    return {**base, 'mode': 'fresh', 'partitions': parts, 'new_userdata_last': new_end,
            'new_userdata_blocks': new_end - userdata['first'] + 1,
            'f2fs': dict(f2fs), 'f2fs_ioctl_target_blocks': (new_end - userdata['first'] + 1) - ALIGN,
            'gpt_writes': True, 'write_order': ['backup_entries', 'backup', 'primary_entries', 'primary'],
            'execution_ready': False,
            'blocked_reason': 'NEW_INSTALL_NOT_READY: compiled/validated Android shrink helper and GPT write transport are absent'}


def _new_header(original, crc):
    data = bytearray(original);struct.pack_into('<I', data, 88, crc);struct.pack_into('<I', data, 16, 0)
    size = struct.unpack_from('<I', data, 12)[0]
    struct.pack_into('<I', data, 16, zlib.crc32(data[:size]))
    return bytes(data)


def fresh_write_schedule(current, plan, ioctl_success, superblocks_after):
    """Pure admission check and buffers; deliberately does not write any device."""
    require(plan['mode'] == 'fresh' and current.baseline() == plan['baseline'], 'GPT changed since fresh plan')
    expected = plan_gpt(current, plan['f2fs'], [row['guid'] for row in plan['partitions']])
    require(expected == plan, 'Fresh plan geometry/identity was altered')
    require(ioctl_success is True, 'Successful F2FS shrink ioctl is required before GPT changes')
    after = f2fs_superblocks(superblocks_after)
    require(after['section_blocks'] == ALIGN and after['block_count'] < plan['f2fs']['block_count'] and
            after['block_count'] <= plan['new_userdata_blocks'],
            'Both F2FS superblocks must fit the new userdata partition')
    table = bytearray(current.primary_entries)
    index = plan['userdata']['index'];struct.pack_into('<Q', table, index * 128 + 40, plan['new_userdata_last'])
    for row in plan['partitions']:
        entry = bytearray(128);entry[:16] = uuid.UUID(row['type']).bytes_le;entry[16:32] = uuid.UUID(row['guid']).bytes_le
        struct.pack_into('<QQQ', entry, 32, row['first'], row['last'], 0)
        encoded = row['name'].encode('utf-16-le');entry[56:56 + len(encoded)] = encoded
        table[row['index'] * 128:(row['index'] + 1) * 128] = entry
    table = bytes(table)
    crc = zlib.crc32(table[:current.geometry['count'] * 128])
    primary, backup = _new_header(current.primary, crc), _new_header(current.backup, crc)
    parse_gpt(primary, table, backup, table, current.disk_bytes)
    return [(current.backup_table_lba, table), (current.disk_bytes // BLOCK - 1, backup),
            (current.geometry['table_lba'], table), (1, primary)]


def _bundle_path(base, name):
    require(isinstance(name, str) and '\\' not in name and ':' not in name and
            not PurePosixPath(name).is_absolute() and all(part not in ('', '.', '..') for part in name.split('/')),
            'Unsafe bundle path')
    path = base
    for part in PurePosixPath(name).parts:
        path /= part
        require(not path.is_symlink(), 'Bundle symlink rejected')
    require(path.is_file(), 'Missing bundle input: ' + name)
    return path


def image_info(path, kind):
    path = Path(path)
    with path.open('rb') as stream:
        head = stream.read(4096)
    require(head[:4] != struct.pack('<I', 0xed26ff3a), 'Android sparse images are not supported')
    size = path.stat().st_size
    require(size > 0 and size % BLOCK == 0, 'Raw image must be aligned to 4 KiB')
    if kind == 'esp':
        require(len(head) >= 512 and head[510:512] == b'\x55\xaa' and head[82:90] == b'FAT32   ', 'Expected raw FAT32 ESP image')
        sector = struct.unpack_from('<H', head, 11)[0]
        sectors = struct.unpack_from('<I', head, 32)[0]
        require(sector in (512, 1024, 2048, 4096) and sectors > 0 and sectors * sector <= size, 'FAT32 filesystem exceeds image')
        label = head[71:82].decode('ascii').rstrip(' ')
    else:
        require(len(head) >= 2048 and head[1080:1082] == b'\x53\xef', 'Expected raw ext4 root image')
        superblock = head[1024:2048]
        blocks = struct.unpack_from('<I', superblock, 4)[0]
        blocks |= struct.unpack_from('<I', superblock, 336)[0] << 32
        log_block = struct.unpack_from('<I', superblock, 24)[0]
        require(log_block <= 6 and 0 < blocks * (1024 << log_block) <= size, 'ext4 filesystem exceeds image')
        label = superblock[120:136].split(b'\0', 1)[0].decode('ascii')
    return {'bytes': size, 'expanded_bytes': size, 'label': label}


def load_bundle(directory, partitions, require_images=False):
    base = Path(directory)
    require(base.is_dir() and not base.is_symlink(), 'Expected a local bundle directory')
    require(not (base / '.incomplete').exists() and not (base / '.incomplete').is_symlink(), 'Bundle is incomplete')
    manifest_path = _bundle_path(base, 'manifest.json')
    manifest_sha = file_sha(manifest_path)
    manifest = read_json(manifest_path)
    require(manifest.get('schema_version') == 1 and manifest.get('status') == 'HOST_BUILT_NOT_DEVICE_READY', 'Unsupported disk bundle manifest')
    require(manifest.get('root_policy') == 'LABEL=PIANOROOT', 'Installer requires portable LABEL=PIANOROOT policy')
    files = manifest.get('files', {})
    missing = [name for name in IMAGE_NAMES if name not in files]
    if missing:
        require(not require_images, 'RAW_IMAGES_REQUIRED: tar-only bundles cannot be fastboot flashed; missing ' + ', '.join(missing))
        require(file_sha(manifest_path) == manifest_sha, 'Bundle manifest changed during inspection')
        return {'manifest_sha256': manifest_sha, 'update_ready': False, 'missing_raw_images': missing}
    images = {}
    for name, kind, partition, expected_label in zip(IMAGE_NAMES, ('esp', 'root'), partitions, ('SUNUEFI_ESP', 'PIANOROOT')):
        record = files[name];path = _bundle_path(base, name)
        info = image_info(path, kind)
        require(type(record.get('bytes')) is int and record['bytes'] == info['bytes'] and
                record.get('expanded_bytes', record['bytes']) == info['bytes'] and
                record.get('format') in (kind, 'FAT32' if kind == 'esp' else 'ext4', 'raw-fat32' if kind == 'esp' else 'raw-ext4'),
                'Bundle raw image format/length differs: ' + name)
        require(info['label'] == expected_label and info['bytes'] <= partition['bytes'], 'Image label/partition capacity mismatch: ' + name)
        require(file_sha(path) == record.get('sha256'), 'Bundle SHA256 mismatch: ' + name)
        images[name] = {'path': path, 'bytes': info['bytes'], 'sha256': record['sha256'], 'partition': partition['name']}
    require(file_sha(manifest_path) == manifest_sha and not (base / '.incomplete').exists(), 'Bundle changed during inspection')
    return {'manifest_sha256': manifest_sha, 'update_ready': True, 'images': images}


class Device:
    def __init__(self, serial, runner=subprocess.run):
        require(re.fullmatch(r'[A-Za-z0-9._:-]+', serial or '') and serial != 'SunUEFI-piano', 'Explicit factory ADB serial required')
        self.serial, self.runner = serial, runner

    def call(self, argv, timeout=60):
        result = self.runner(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, check=False)
        require(result.returncode == 0, 'Command failed: ' + ' '.join(argv[:4]) + ': ' + result.stderr.decode(errors='replace')[-1000:])
        return result.stdout, result.stderr

    def shell(self, command):
        return self.call(['adb', '-s', self.serial, 'exec-out', 'su -c ' + shlex.quote(command)])[0]

    def text(self, command):
        return self.shell(command).decode().strip()

    def read(self, path, first, count):
        require(re.fullmatch(r'/dev/block/(?:sd[a-z]+\d*|by-name/(?:userdata|sunuefi_esp|sunuefi_root))', path), 'Unsafe block path')
        require(type(first) is int and first >= 0 and type(count) is int and 0 < count <= 256, 'Unbounded metadata read')
        data = self.shell(f'dd if={path} bs={BLOCK} skip={first} count={count} 2>/dev/null')
        require(len(data) == count * BLOCK, 'Incomplete block read')
        return data

    def inspect(self):
        require(self.text('id -u') == '0', 'Rooted ADB is required')
        require(self.text('getprop ro.product.device') == 'piano' and self.text('getprop ro.boot.flash.locked') == '0', 'Expected unlocked piano')
        factory = self.text('getprop ro.serialno')
        require(re.fullmatch(r'[A-Za-z0-9._:-]+', factory or '') and factory != 'SunUEFI-piano', 'Factory serial missing')
        userdata_path = self.text('readlink -f /dev/block/by-name/userdata')
        match = re.fullmatch(r'/dev/block/(sd[a-z]+)(\d+)', userdata_path)
        require(match is not None, 'Userdata must resolve to an explicit UFS partition')
        parent, part_number = match[1], int(match[2]);disk = '/dev/block/' + parent
        scsi = self.text(f'readlink -f /sys/class/block/{parent}/device').rsplit('/', 1)[-1]
        require(re.fullmatch(r'\d+:\d+:\d+:0', scsi), 'Userdata must reside on UFS LUN0')
        require(self.text('blockdev --getss ' + disk) == str(BLOCK), 'Only 4 KiB logical sectors are supported')
        disk_bytes = int(self.text('blockdev --getsize64 ' + disk))
        primary, backup = self.read(disk, 1, 1), self.read(disk, disk_bytes // BLOCK - 1, 1)
        front = gpt_header(primary, 1, disk_bytes // BLOCK - 1)
        rear = gpt_header(backup, disk_bytes // BLOCK - 1, disk_bytes // BLOCK - 1)
        gpt = parse_gpt(primary, self.read(disk, front['table_lba'], front['table_blocks']),
                        backup, self.read(disk, rear['table_lba'], rear['table_blocks']), disk_bytes)
        userdata = next(row for row in gpt.partitions if row['name'] == 'userdata')
        require(userdata['index'] + 1 == part_number and
                int(self.text(f'cat /sys/class/block/{parent}{part_number}/start')) * 512 == userdata['first'] * BLOCK and
                int(self.text('blockdev --getsize64 ' + userdata_path)) == userdata['bytes'], 'Kernel userdata view differs from GPT')
        raw_superblocks = self.read(userdata_path, 0, 2)
        try:
            f2fs = f2fs_superblocks(raw_superblocks)
        except ValueError:
            f2fs = None
        identity = {'product': 'piano', 'unlocked': True, 'factory_serial': factory,
                    'disk': disk, 'disk_bytes': disk_bytes, 'lun': 0, 'block_bytes': BLOCK}
        return {'identity': identity, 'gpt': gpt, 'f2fs': f2fs}

    def reboot_bootloader(self):
        self.call(['adb', '-s', self.serial, 'reboot', 'bootloader'])

    def fastboot_var(self, factory, key):
        stdout, stderr = self.call(['fastboot', '-s', factory, 'getvar', key])
        matches = re.findall(r'(?m)^(?:\(bootloader\)\s*)?' + re.escape(key) + r':\s*([^\r\n]+)', (stdout + stderr).decode())
        require(len(matches) == 1, 'Factory fastboot variable missing/ambiguous: ' + key)
        return matches[0].strip()

    def check_fastboot(self, identity, partition):
        serial = identity['factory_serial']
        require(self.fastboot_var(serial, 'serialno') == serial and self.fastboot_var(serial, 'product') == 'piano' and
                self.fastboot_var(serial, 'unlocked') == 'yes' and self.fastboot_var(serial, 'is-userspace') == 'no',
                'Factory bootloader identity/unlocked mode differs')
        size = self.fastboot_var(serial, 'partition-size:' + partition['name'])
        require(int(size, 16) == partition['bytes'], 'Factory partition capacity differs from inspected GPT')

    def flash(self, identity, partition, image):
        require(partition['name'] in PARTITIONS, 'Only dedicated OS partitions may be flashed')
        self.check_fastboot(identity, partition)
        self.call(['fastboot', '-s', identity['factory_serial'], 'flash', partition['name'], str(image)], timeout=1800)

    def reboot_android(self, identity):
        self.call(['fastboot', '-s', identity['factory_serial'], 'reboot'])
        self.call(['adb', '-s', self.serial, 'wait-for-device'], timeout=300)

    def readback_hash(self, name, size):
        require(name in PARTITIONS and size % BLOCK == 0, 'Readback limited to dedicated OS image prefixes')
        output = self.text(f'dd if=/dev/block/by-name/{name} bs={BLOCK} count={size // BLOCK} 2>/dev/null | sha256sum')
        require(re.fullmatch(r'[0-9a-f]{64}\s+.*', output), 'Device SHA256 readback failed')
        return output.split()[0]


def make_plan(snapshot, bundle_dir=None, new_guids=None):
    geometry = plan_gpt(snapshot['gpt'], snapshot['f2fs'], new_guids)
    plan = {'schema_version': 1, 'status': 'READ_ONLY_PLAN', 'device': snapshot['identity'], 'gpt': geometry}
    if bundle_dir is not None:
        bundle = load_bundle(bundle_dir, geometry['partitions'])
        plan['bundle'] = {key: value for key, value in bundle.items() if key != 'images'}
        if bundle.get('images'):
            plan['bundle']['images'] = {name: {key: value for key, value in image.items() if key != 'path'} for name, image in bundle['images'].items()}
    return plan


def inspection_report(snapshot):
    gpt = snapshot['gpt']
    selected = [dict(row) for row in gpt.partitions if row['name'] in ('userdata', *PARTITIONS)]
    return {'schema_version': 1, 'status': 'READ_ONLY_INSPECTION', 'device': snapshot['identity'],
            'gpt_baseline': gpt.baseline(), 'partition_count': len(gpt.partitions),
            'selected_partitions': selected, 'f2fs': snapshot['f2fs'], 'device_writes': False}


def apply_update(device, plan, bundle_dir, execute=False, recovery=False):
    require(not recovery, 'RECOVERY_INSTALL_NOT_READY: recovery writes are unavailable')
    require(plan.get('schema_version') == 1 and plan.get('status') == 'READ_ONLY_PLAN', 'Expected a reviewed read-only plan')
    if plan['gpt']['mode'] == 'fresh':
        if execute:
            raise ValueError(plan['gpt']['blocked_reason'])
        return {'status': 'NEW_INSTALL_NOT_READY', 'device_writes': False, 'reason': plan['gpt']['blocked_reason']}
    snapshot = device.inspect()
    fresh = make_plan(snapshot, bundle_dir)
    require(fresh == plan, 'Device GPT/identity or bundle changed since plan; inspect and plan again')
    bundle = load_bundle(bundle_dir, plan['gpt']['partitions'], require_images=True)
    if not execute:
        return {'status': 'UPDATE_HOST_AND_GPT_VALIDATED_NOT_EXECUTED', 'device_writes': False,
                'factory_fastboot_checks_pending': True, 'targets': list(PARTITIONS)}
    device.reboot_bootloader()
    for name, partition in zip(IMAGE_NAMES, plan['gpt']['partitions']):
        image = bundle['images'][name]
        require(file_sha(image['path']) == image['sha256'], 'Image changed before flash: ' + name)
        device.flash(snapshot['identity'], partition, image['path'])
        require(file_sha(image['path']) == image['sha256'], 'Image changed during flash: ' + name)
    device.reboot_android(snapshot['identity'])
    after = device.inspect()
    require(after['identity'] == snapshot['identity'] and after['gpt'].baseline() == snapshot['gpt'].baseline(),
            'Post-update device/GPT differs; readback incomplete')
    for name in IMAGE_NAMES:
        image = bundle['images'][name]
        require(device.readback_hash(image['partition'], image['bytes']) == image['sha256'], 'Device image readback mismatch: ' + name)
    return {'status': 'UPDATE_PREFIX_READBACK_VERIFIED', 'targets': list(PARTITIONS),
            'device_writes': True, 'gpt_changed': False, 'linux_boot_verified': False, 'recovery_written': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('inspect', 'plan', 'apply'), nargs='?', default='inspect')
    parser.add_argument('--serial', required=True, help='Explicit rooted Android ADB serial; factory serial is rechecked')
    parser.add_argument('--bundle', type=Path, help='Host disk bundle for plan/apply, containing manifest.json')
    parser.add_argument('--plan', type=Path, help='Reviewed JSON plan for apply')
    parser.add_argument('--output', type=Path, help='Write inspection/plan/result JSON; never a partition backup')
    parser.add_argument('--execute', action='store_true', help='Allow existing dedicated ESP/Linux flash and verified readback')
    parser.add_argument('--recovery', action='store_true', help='Explicit recovery request; currently rejected as not ready')
    args = parser.parse_args()
    try:
        require(not args.execute or args.operation == 'apply', '--execute is only valid for apply')
        require(not args.recovery, 'RECOVERY_INSTALL_NOT_READY: no recovery operation is implemented')
        require(args.operation != 'inspect' or args.bundle is None, 'Use plan --bundle to verify a host bundle')
        if args.output:
            require(not args.output.exists() and not args.output.is_symlink(), 'Output exists; refusing to replace it')
        device = Device(args.serial)
        if args.operation == 'apply':
            require(args.plan is not None and args.bundle is not None, 'apply requires --plan and --bundle')
            result = apply_update(device, read_json(args.plan), args.bundle, args.execute)
        else:
            snapshot = device.inspect()
            result = inspection_report(snapshot) if args.operation == 'inspect' else make_plan(snapshot, args.bundle)
        text = json.dumps(result, indent=2, sort_keys=True) + '\n'
        if args.output:
            with args.output.open('x') as stream:
                stream.write(text)
        print(text, end='')
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
