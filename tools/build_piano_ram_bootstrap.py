#!/usr/bin/env python3
"""Build the real ARM64 GNU-tar bootstrap around a frozen complete root payload.

Streaming newc carries the compressed tar as data, avoiding double compression
or multi-GB Python allocations. No firmware build, device access or boot claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import struct
import subprocess

from make_kernel_initramfs import validate_static_arm64_elf
from package_piano_ram_root import ROOT, LIMIT, sha_file

BUSYBOX_SHA = '999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22'
APPLETS = ('sh', 'cat', 'mkdir', 'mount', 'mountpoint', 'awk', 'sha256sum',
           'chmod', 'uname', 'chroot', 'switch_root', 'gzip')


def guest_resolve(root, name):
    """Resolve guest absolute symlinks within that guest, never the host root."""
    root = Path(root).resolve()
    pending = name.strip('/').split('/'); parts = []; hops = 0
    while pending:
        part = pending.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            if not parts:
                raise ValueError('Guest link escapes root')
            parts.pop(); continue
        candidate = root.joinpath(*parts, part)
        if candidate.is_symlink():
            hops += 1
            if hops > 40:
                raise ValueError('Guest symlink cycle')
            target = str(candidate.readlink())
            if target.startswith('/'):
                parts = []
            pending = target.split('/') + pending
        else:
            parts.append(part)
    return root.joinpath(*parts)


def elf_dependencies(path):
    header = Path(path).read_bytes()[:64]
    if len(header) < 64 or header[:5] != b'\x7fELF\x02' or struct.unpack_from('<H', header, 18)[0] != 183:
        raise ValueError('Expected ELF64 AArch64 bootstrap executable/library')
    dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    program = subprocess.check_output(['readelf', '-l', str(path)], text=True)
    needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic)
    interpreter = re.findall(r'Requesting program interpreter: ([^\]]+)\]', program)
    return needed, interpreter


def runtime_files(root):
    pending = ['/usr/bin/tar']; files = {}
    while pending:
        name = pending.pop(0)
        if name in files:
            continue
        source = guest_resolve(root, name)
        if not source.is_file():
            raise ValueError('Missing GNU tar runtime dependency: ' + name)
        needed, interpreters = elf_dependencies(source)
        files[name.lstrip('/')] = source
        pending += interpreters
        for soname in needed:
            if '/' in soname:
                raise ValueError('Unexpected ELF dependency path')
            pending.append('/lib/aarch64-linux-gnu/' + soname)
    return files


def write_newc(stream, name, mode, inode, data=None, source=None):
    if (not name or name.startswith('/') or '\0' in name or '..' in name.split('/')
            or (data is None) == (source is None)):
        raise ValueError('Invalid archive entry')
    length = len(data) if source is None else Path(source).stat().st_size
    raw_name = name.encode() + b'\0'
    fields = [inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0, length,
              0, 0, 0, 0, len(raw_name), 0]
    if any(not 0 <= value <= 0xffffffff for value in fields):
        raise ValueError('newc field exceeds its protocol limit')
    stream.write(b'070701' + b''.join(f'{value:08x}'.encode() for value in fields))
    stream.write(raw_name); stream.write(b'\0' * (-stream.tell() % 4))
    if source is not None:
        with Path(source).open('rb') as incoming:
            copied = 0
            for chunk in iter(lambda: incoming.read(1024**2), b''):
                copied += len(chunk); stream.write(chunk)
            if copied != length:
                raise ValueError('Source size changed while archiving')
    else:
        stream.write(data)
    stream.write(b'\0' * (-stream.tell() % 4))


def build(root, payload_dir, busybox, output):
    root, payload_dir, busybox, output = map(lambda p: Path(p).resolve(), (root, payload_dir, busybox, output))
    if not root.is_relative_to(ROOT / 'build/distros') or not payload_dir.is_relative_to(ROOT / 'artifacts'):
        raise ValueError('Expected explicit workspace distro and root payload')
    if not output.is_relative_to(ROOT / 'artifacts') or output.exists():
        raise ValueError('Bootstrap output must be a fresh artifact directory')
    payload = payload_dir / 'Piano-rootfs.tar.gz'
    record = json.loads((payload_dir / 'manifest.json').read_text())
    if record.get('status') != 'HOST_PACKAGED_GNU_ROOT_NOT_BOOT_VERIFIED' or record.get('root_limit_bytes') != LIMIT:
        raise ValueError('Expected sealed 4GiB GNU root payload')
    if payload.stat().st_size != record['archive_bytes'] or sha_file(payload) != record['archive_sha256']:
        raise ValueError('Root payload differs from its sealed manifest')
    budget = record['regular_page_budget_bytes']
    if not 0 < budget < LIMIT:
        raise ValueError('Invalid expanded root budget')
    if sha_file(busybox) != BUSYBOX_SHA:
        raise ValueError('Static BusyBox differs from the known source artifact')
    validate_static_arm64_elf(busybox.read_bytes())
    files = runtime_files(root)
    # Verify all copied runtime members and preserve the real ARM64 tar closure.
    rows = {name: {'bytes': path.stat().st_size, 'sha256': sha_file(path)} for name, path in files.items()}
    dirs = {'bin', 'usr', 'usr/bin', 'dev', 'proc', 'sys', 'run', 'tmp', 'sysroot'}
    for name in files:
        dirs.update(parent.as_posix() for parent in Path(name).parents if parent.as_posix() != '.')
    output.mkdir(parents=True)
    archive = output / 'initramfs.cpio'
    inode = 1
    with archive.open('xb') as stream:
        for name in sorted(dirs, key=lambda value: (value.count('/'), value)):
            write_newc(stream, name, stat.S_IFDIR | (0o1777 if name == 'tmp' else 0o755), inode, data=b''); inode += 1
        write_newc(stream, 'bin/busybox', stat.S_IFREG | 0o755, inode, source=busybox); inode += 1
        for applet in APPLETS:
            write_newc(stream, 'bin/' + applet, stat.S_IFLNK | 0o777, inode, data=b'busybox'); inode += 1
        for name, source in sorted(files.items()):
            write_newc(stream, name, stat.S_IFREG | 0o755, inode, source=source); inode += 1
        entry = ROOT / 'linux/userspace/ram-bootstrap'
        write_newc(stream, 'pianoinit', stat.S_IFREG | 0o755, inode, source=entry); inode += 1
        check = (record['archive_sha256'] + '  rootfs.tar.gz\n').encode()
        write_newc(stream, 'rootfs.sha256', stat.S_IFREG | 0o600, inode, data=check); inode += 1
        write_newc(stream, 'rootfs.page-budget', stat.S_IFREG | 0o600, inode, data=(str(budget) + '\n').encode()); inode += 1
        write_newc(stream, 'rootfs.tar.gz', stat.S_IFREG | 0o600, inode, source=payload); inode += 1
        write_newc(stream, 'TRAILER!!!', 0, inode, data=b'')
        stream.write(b'\0' * (-stream.tell() % 512))
    if sha_file(payload) != record['archive_sha256'] or any(sha_file(files[name]) != row['sha256'] for name, row in rows.items()):
        raise ValueError('Runtime or payload changed during bootstrap construction')
    result = {'status': 'HOST_BUILT_GNU_ROOT_BOOTSTRAP_NOT_BOOT_VERIFIED',
              'initramfs_bytes': archive.stat().st_size, 'initramfs_sha256': sha_file(archive),
              'root_payload_sha256': record['archive_sha256'], 'root_limit_bytes': LIMIT,
              'expanded_root_page_budget_bytes': budget, 'gnu_tar_runtime': rows,
              'busybox_sha256': BUSYBOX_SHA, 'entry_sha256': sha_file(entry),
              'tool_sha256': sha_file(__file__), 'root_extraction_passes': 1,
              'piano_boot_verified': False, 'hardware_prepare_required': True,
              'firmware_download_limit_changed': False}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, default=ROOT / 'build/linux-ram/busybox')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.rootfs, args.payload, args.busybox, args.output), indent=2))
