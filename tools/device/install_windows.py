"""Install ARM64 Windows media onto the two dedicated Piano partitions."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import uuid

from install_piano import (BASIC_TYPE, BLOCK, ESP_TYPE, LINUX_TYPE, PARTITIONS, Device,
                           _bundle_path, discover_serial, file_sha, read_json, require)

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024**2
LOADER = 'EFI/Microsoft/Boot/bootmgfw.efi'
TEMPLATES = ROOT / 'windows/deployment'


def dedicated(gpt):
    named = {p['name']: p for p in gpt.partitions}
    require(all(n in named for n in PARTITIONS), 'Create the dedicated ESP and system partition first')
    esp, system = (named[n] for n in PARTITIONS)
    require(uuid.UUID(esp['type']).bytes_le == ESP_TYPE and
            uuid.UUID(system['type']).bytes_le in (LINUX_TYPE, BASIC_TYPE),
            'The named ESP or system partition has an unexpected type')
    return esp, system


def layout(gpt, esp_mib, install):
    esp, system = dedicated(gpt)
    size = esp_mib * MIB if esp_mib is not None else esp['bytes']
    require(size > 0, 'ESP size must be positive')
    if size != esp['bytes']:
        require(esp['last'] + 1 == system['first'],
                'Moving the boundary requires an ESP immediately followed by the dedicated system partition')
        end = esp['first'] + size // BLOCK - 1
        require(end < system['last'], 'ESP does not leave room for the Windows partition')
        esp = {**esp, 'last': end, 'bytes': size}
        system = {**system, 'first': end + 1, 'bytes': (system['last'] - end) * BLOCK}
    if install:
        system = {**system, 'type': str(uuid.UUID(bytes_le=BASIC_TYPE))}
    return esp, system


def gpt_command(disk, old, new):
    changed = [(a, b) for a, b in zip(old, new) if a != b]
    if not changed:
        return []
    args = ['sgdisk', '--set-alignment=1']
    moved = [b for a, b in changed if (a['first'], a['last']) != (b['first'], b['last'])]
    # Remove both old extents before installing either of the new ones.
    args += [f'--delete={p["index"] + 1}' for p in moved]
    for a, b in changed:
        i = b['index'] + 1
        if b in moved:
            args += [f'--new={i}:{b["first"]}:{b["last"]}', f'--change-name={i}:{b["name"]}',
                     f'--partition-guid={i}:{b["guid"]}', f'--attributes={i}:=:{b["attributes"]:016x}']
        args.append(f'--typecode={i}:{b["type"]}')
    return [*args, disk]


def verify_gpt(before, after, intended):
    expected = {p['name']: p for p in intended}
    require(before.disk_bytes == after.disk_bytes and
            before.geometry['disk_guid'] == after.geometry['disk_guid'] and
            tuple(expected.get(p['name'], p) for p in before.partitions) == after.partitions,
            'GPT readback differs; no filesystem images have been written')


class StalePartitionView(ValueError):
    """The kernel still has partition extents from before the GPT edit."""


def inspect(device):
    require(device.text('id -u') == '0' and device.text('getprop ro.product.device') == 'piano',
            'Connect the Piano tablet in Android with rooted ADB')
    nodes = [device.text(f'readlink -f /dev/block/by-name/{n}') for n in PARTITIONS]
    parts = [re.fullmatch(r'/dev/block/(sd[a-z]+)([0-9]+)', n) for n in nodes]
    require(all(parts) and parts[0][1] == parts[1][1], 'The dedicated partitions must be on one UFS disk')
    disk = '/dev/block/' + parts[0][1]
    gpt = device.read_gpt(disk, require_userdata=False)
    for match, node, p in zip(parts, nodes, dedicated(gpt)):
        base = node.rsplit('/', 1)[-1]
        if (int(match[2]) != p['index'] + 1 or
                int(device.text(f'cat /sys/class/block/{base}/start')) * 512 != p['first'] * BLOCK or
                int(device.text('blockdev --getsize64 ' + node)) != p['bytes']):
            raise StalePartitionView('Android has not re-read its partition table; restart Android before writing')
    serial = device.text('getprop ro.serialno')
    require(serial and serial != 'SunUEFI-piano', 'Factory serial unavailable')
    return {'factory_serial': serial, 'disk': disk}, gpt


def media_files(directory):
    data = read_json(directory / 'media.json')
    require(data.get('format') == 'sunuefi-windows-media' and data.get('loader') == LOADER,
            'Use the output of ./build.sh windows media')
    files = data['files']
    for name, digest in files.items():
        path = _bundle_path(directory, name)
        require(name.startswith(('esp/', 'ntfs/')) and file_sha(path) == digest,
                'Unrecognized or changed media file: ' + name)
        if name.startswith('esp/'):
            dest = name.removeprefix('esp/').lower()
            require(dest.startswith(('efi/microsoft/', 'sunuefi/winpe/')) or
                    dest in ('boot/boot.sdi', 'sources/boot.wim'),
                    'Media would overwrite a file outside the Windows boot files: ' + name)
    require('esp/' + LOADER in files and 'esp/sources/boot.wim' in files, 'Incomplete WinPE media')
    return data


def run(args, **kwargs):
    return subprocess.run([os.fspath(a) for a in args], check=True, **kwargs)


def progress(text):
    print(text, file=sys.stderr, flush=True)


def copy_tree_to_fat(image, tree, files, replacements):
    dirs = set()
    for rel in files:
        target = Path(rel)
        for parent in reversed(target.parents):
            if parent != Path('.') and parent not in dirs:
                dest = '::/' + parent.as_posix()
                if subprocess.run(['mdir', '-b', '-i', image, dest], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode:
                    run(['mmd', '-i', image, dest])
                dirs.add(parent)
        run(['mcopy', '-om', '-i', image, replacements.get(rel, tree / rel), '::/' + rel])


def build_esp(original, output, partition, added_files, media_dir, replacements=None):
    with original.open('rb') as stream:
        bpb = stream.read(512)
    require(bpb[82:90] == b'FAT32   ' and bpb[510:512] == b'\x55\xaa', 'Existing ESP is not FAT32')
    run(['fsck.fat', '-n', original], stdout=subprocess.DEVNULL)
    if partition['bytes'] == original.stat().st_size:
        shutil.copyfile(original, output)
    else:
        with output.open('xb') as stream:
            stream.truncate(partition['bytes'])
        label = bpb[71:82].decode('ascii').rstrip()
        serial = f'{int.from_bytes(bpb[67:71], "little"):08x}'
        args = ['mkfs.fat', '-F', '32', '-S', str(BLOCK), '-s', '1', '-i', serial,
                '-h', str(partition['first'])]
        if label and label != 'NO NAME':
            args += ['-n', label]
        run([*args, output], stdout=subprocess.DEVNULL)
        # mtools copies directly between FAT volumes, keeping DOS attributes,
        # timestamps, long names and files not owned by the Windows installer.
        with tempfile.NamedTemporaryFile(mode='w', dir=output.parent) as config:
            for drive, image in (('a', original), ('b', output)):
                name = str(image.resolve()).replace('\\', '\\\\').replace('"', '\\"')
                config.write(f'drive {drive}: file="{name}"\n')
            config.flush()
            env = {**os.environ, 'MTOOLSRC': config.name}
            roots = run(['mdir', '-a', '-b', 'a:/'], env=env, capture_output=True, text=True)
            for entry in roots.stdout.splitlines():
                run(['mcopy', '-spm', entry.rstrip('/'), 'b:/'], env=env)
    copy_tree_to_fat(output, media_dir / 'esp', added_files, replacements or {})
    run(['fsck.fat', '-n', output], stdout=subprocess.DEVNULL)


def root_sources(media, directory, edition):
    install = media['install']
    selected = [i for i in install['images'] if str(i['index']) == edition or i['edition'] == edition]
    require(len(selected) == 1, 'Choose one Windows edition or image index from media.json')
    require(type(selected[0]['index']) is int and selected[0]['index'] > 0, 'Invalid WIM index')
    first = 'ntfs/' + install['path']
    require(first in media['files'], 'Missing installation image')
    ext = Path(first).suffix.lower()
    require(ext in ('.wim', '.esd', '.swm'), 'Unknown Windows image format')
    sources = [(directory / first, 'SunUEFI-install' + ext)]
    if ext == '.swm':
        for path in media['files']:
            sibling = Path(path)
            m = re.fullmatch(r'install([2-9]|[1-9][0-9]+)\.swm', sibling.name, re.I)
            if sibling.parent == Path(first).parent and m:
                sources.append((directory / path, 'SunUEFI-install' + m[1] + '.swm'))
    return selected[0], sources


def install_script(marker, name, index):
    template = (TEMPLATES / 'install.cmd').read_text(encoding='utf-8')
    command = (template.replace('@TARGET_MARKER@', marker).replace('@IMAGE_NAME@', name)
               .replace('@IMAGE_INDEX@', str(index)))
    return command.replace('\n', '\r\n').encode('utf-8')


def stage_full(work, partition, directory, media, edition):
    selected, sources = root_sources(media, directory, edition)
    require(sum(p.stat().st_size for p, _ in sources) < partition['bytes'], 'Windows media exceeds system volume')
    token = uuid.uuid4().hex
    marker = 'SunUEFI-' + token
    tag = work / 'target.tag'
    tag.write_text(token)
    root = work / 'windows.ntfs'
    with root.open('xb') as stream:
        stream.truncate(partition['bytes'])
    run(['mkntfs', '-F', '-Q', '-q', '-s', str(BLOCK), '-c', str(BLOCK), '-H', '255', '-S', '63',
         '-p', str(partition['first']), '-L', 'Windows', root])
    for source, name in [*sources, (tag, marker + '.target')]:
        run(['ntfscp', '-q', root, source, '/' + name])
    # ntfsclone copies allocated clusters (also initialized zero data), and
    # makes holes for free space. Do not turn old Linux free space into a
    # 100+ GiB upload or issue a discard against the whole disk.
    used = work / 'windows-used.ntfs'
    run(['ntfsclone', '--quiet', '--full-logfile', '--output', used, root])
    root.unlink()
    wim = work / 'boot.wim'
    shutil.copyfile(directory / 'esp/sources/boot.wim', wim)
    (work / 'install.cmd').write_bytes(install_script(marker, sources[0][1], selected['index']))
    ini = work / 'winpeshl.ini'
    ini.write_bytes(b'[LaunchApps]\r\n%SYSTEMROOT%\\System32\\cmd.exe, "/d /c %SYSTEMROOT%\\System32\\sunuefi-install.cmd"\r\n')
    run(['wimlib-imagex', 'update', wim, str(media['boot']['index'])], cwd=work, text=True,
        input='add winpeshl.ini /Windows/System32/winpeshl.ini\n'
              'add install.cmd /Windows/System32/sunuefi-install.cmd\n', stdout=subprocess.DEVNULL)
    return used, {'sources/boot.wim': wim, f'SunUEFI/Deployment/{marker}.pending': tag}


def used_ranges(image):
    """Ranges sent by img2simg -s on an NTFS clone, including allocated zero data."""
    with image.open('rb') as stream:
        pos, size = 0, image.stat().st_size
        while pos < size:
            try:
                start = os.lseek(stream.fileno(), pos, os.SEEK_DATA)
            except OSError as exc:
                if exc.errno != errno.ENXIO:
                    raise
                return
            pos = min(os.lseek(stream.fileno(), start, os.SEEK_HOLE), size)
            require(start % BLOCK == pos % BLOCK == 0, 'Unaligned sparse filesystem extent')
            yield start, pos - start


def verify_root(device, image):
    with image.open('rb') as stream:
        for start, count in used_ranges(image):
            stream.seek(start)
            value = hashlib.sha256()
            left = count
            while left:
                buf = stream.read(min(left, 4 * MIB))
                require(bool(buf), 'Incomplete host NTFS extent')
                value.update(buf)
                left -= len(buf)
            block = 4 * MIB if (start | count) % (4 * MIB) == 0 else BLOCK
            cmd = (f'dd if=/dev/block/by-name/sunuefi_root bs={block} skip={start // block} '
                   f'count={count // block} 2>/dev/null | sha256sum')
            out = device.shell(cmd, timeout=max(60, count // (8 * MIB))).decode().split()
            require(out and out[0] == value.hexdigest(), f'NTFS readback failed at byte {start}')


def unmounted(device):
    for name in PARTITIONS:
        dev = device.text(f'readlink -f /dev/block/by-name/{name}').rsplit('/', 1)[-1]
        require(re.fullmatch(r'sd[a-z]+[0-9]+', dev), 'Unexpected UFS partition device')
        num = device.text(f'cat /sys/class/block/{dev}/dev')
        mounts = device.text('cat /proc/self/mountinfo /proc/1/mountinfo')
        require(not any(row.split()[2] == num for row in mounts.splitlines() if row) and
                not device.text(f'ls /sys/class/block/{dev}/holders'), f'Unmount {name} before installation')


def wait_android(device):
    device.call(['adb', '-s', device.serial, 'shell',
                 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 1; done'], timeout=240)


def android_partition_view(device, previous, intended):
    try:
        identity, result = inspect(device)
    except StalePartitionView:
        # Android may keep the old partition nodes while userdata is mounted.
        # A normal reboot re-reads GPT without touching the saved boot route.
        device.call(['adb', '-s', device.serial, 'reboot'])
        device.call(['adb', '-s', device.serial, 'wait-for-device'], timeout=300)
        wait_android(device)
        identity, result = inspect(device)
    verify_gpt(previous, result, intended)
    return identity


def android_recovery_route(device):
    if device.text('test ! -x /data/adb/modules/piano_sunuefi/manager.sh || echo installed'):
        status = json.loads(device.text('sh /data/adb/modules/piano_sunuefi/manager.sh quick-status'))
        require(not status['wrapped'] or status['request_target'] == 0,
                'Select Android as the normal boot target before starting the installer')


def deploy(device, directory, esp_mib, edition, execute, workspace):
    media = media_files(directory)
    install = 'install' in media
    require(install != bool(media.get('probe')), 'Choose either WinPE probe media or full installation media')
    identity, gpt = inspect(device)
    before = dedicated(gpt)
    parts = layout(gpt, esp_mib, install)
    esp, system = parts
    files = [p.removeprefix('esp/') for p in media['files'] if p.startswith('esp/')]
    minimum = sum((directory / 'esp' / p).stat().st_size for p in files)
    require(minimum < esp['bytes'], f'WinPE files alone need {minimum // MIB + 1} MiB; '
            'choose a larger --esp-size-mib to replace the adjacent system volume')
    action = {'esp_mib': esp['bytes'] // MIB, 'system_mib': system['bytes'] // MIB,
              'esp_partuuid': esp['guid'], 'system_partuuid': system['guid'],
              'disk_guid': gpt.geometry['disk_guid'], 'replace_system': install or before[1]['first'] != system['first'],
              'gpt_change': before != parts, 'install': root_sources(media, directory, edition)[0] if install else 'WinPE',
              'device_written': False}
    if not execute:
        return action
    if install or before != parts:
        android_recovery_route(device)
    workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='install-', dir=workspace) as temp:
        work = Path(temp).resolve()
        original, final = work / 'original.fat', work / 'esp.fat'
        unmounted(device)
        progress('读取已有 ESP，保留其中的启动文件。')
        with original.open('xb') as stream:
            run(['adb', '-s', device.serial, 'exec-out', 'su -c ' +
                 shlex.quote('dd if=/dev/block/by-name/sunuefi_esp bs=1M 2>/dev/null')], stdout=stream)
        require(original.stat().st_size == before[0]['bytes'], 'Incomplete ESP transfer; disk unchanged')
        if install:
            progress(f'在电脑上准备 NTFS 系统卷和 Windows {action["install"]["version"]} 安装源。')
        root, replacements = stage_full(work, system, directory, media, edition) if install else (None, {})
        build_esp(original, final, esp, list(dict.fromkeys([*files, *replacements])), directory, replacements)

        sparse = None
        if root:
            sparse = work / 'windows.sparse'
            run(['img2simg', '-s', root, sparse, str(BLOCK)])
        now, again = inspect(device)
        require(now == identity and again.baseline() == gpt.baseline(), 'Disk changed while preparing images')
        unmounted(device)
        require(device.readback_hash(PARTITIONS[0], original.stat().st_size) == file_sha(original),
                'ESP files changed while preparing images; nothing was written')
        cmd = gpt_command(identity['disk'], before, parts)
        if cmd:
            device.shell(shlex.join(cmd[:1] + ['--pretend'] + cmd[1:]))
        retry = workspace / ('esp-' + esp['guid'] + '.img')
        require(not retry.exists(), f'An unverified ESP image already exists at {retry}')
        if cmd:
            progress('只更新 SunUEFI 两个独立分区的 GPT 记录，随后读回核对。')
            device.shell(shlex.join(cmd))
            actual = device.read_gpt(identity['disk'], require_userdata=False)
            verify_gpt(gpt, actual, parts)

        if root:
            # The bootloader always re-reads the GPT. A sparse download avoids
            # transmitting the entire free area of a large NTFS volume.
            device.reboot_bootloader()
            device.check_fastboot(identity, system)
            progress('正在向独立系统卷写入稀疏 NTFS 镜像。')
            run(['fastboot', '-s', identity['factory_serial'], '-S', '128M',
                 'flash', system['name'], sparse], timeout=1800)
            device.reboot_android(identity)
            wait_android(device)
        else:
            require(android_partition_view(device, gpt, parts) == identity, 'Device changed before ESP write')
        now, result = inspect(device)
        require(now == identity, 'Device identity changed before readback')
        verify_gpt(gpt, result, parts)
        if root:
            progress('Android 已重新上线，正在读回 NTFS 已写入的数据。')
            verify_root(device, root)
        unmounted(device)
        # The existing FAT filesystem remains intact until the replacement
        # can be written. If USB disconnects halfway through, the only
        # complete copy is the local image; keep it until readback succeeds.
        final.rename(retry)
        try:
            progress(f'写入并校验 {esp["bytes"] // MIB} MiB 的 ESP。')
            with retry.open('rb') as stream:
                run(['adb', '-s', device.serial, 'shell', '-T', 'su -c ' +
                     shlex.quote('dd of=/dev/block/by-name/sunuefi_esp bs=1M conv=fsync')],
                    stdin=stream, timeout=1800)
            require(device.readback_hash(esp['name'], retry.stat().st_size) == file_sha(retry),
                    'ESP readback failed')
        except (ValueError, OSError, subprocess.SubprocessError) as exc:
            raise ValueError(f'ESP was not verified. Its replacement remains at {retry}: {exc}') from exc
        retry.unlink()
    return {**action, 'device_written': True, 'next': '\\EFI\\Microsoft\\Boot\\bootmgfw.efi'}


def main(argv=None):
    parser = argparse.ArgumentParser(prog='./build.sh install windows',
                                     description='Stage WinPE or deploy Windows to the existing SunUEFI volumes.')
    parser.add_argument('--media', type=Path, required=True, help='Output of ./build.sh windows media')
    parser.add_argument('--serial', help='ADB serial (default: the single connected Piano)')
    parser.add_argument('--esp-size-mib', type=int, help='Move only the ESP/system boundary; this erases the system')
    parser.add_argument('--edition', default='Professional', help='Windows edition ID or WIM index')
    parser.add_argument('--execute', action='store_true', help='Write the displayed layout and installation media')
    args = parser.parse_args(argv)
    try:
        result = deploy(Device(args.serial or discover_serial()), args.media.resolve(), args.esp_size_mib,
                        args.edition, args.execute, ROOT / 'build/windows-install')
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'{exc}\n')
