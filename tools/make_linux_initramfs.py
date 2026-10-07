#!/usr/bin/env python3
"""Create a minimal ARM64 initramfs on the host; never contacts a device."""
import gzip
import hashlib
import json
from pathlib import Path
import stat
import struct

def main():
    root = Path(__file__).resolve().parent.parent
    staging = root / 'build/linux-ram'
    busybox = (staging / 'busybox').read_bytes()
    if busybox[:4] != b'\x7fELF' or struct.unpack_from('<H', busybox, 18)[0] != 183:
        raise SystemExit('Busybox must be an ARM64 ELF')
    phoff = struct.unpack_from('<Q', busybox, 32)[0]
    phsize, count = struct.unpack_from('<HH', busybox, 54)
    if any(struct.unpack_from('<I', busybox, phoff + i * phsize)[0] == 3 for i in range(count)):
        raise SystemExit('Busybox has a dynamic interpreter')
    module_dir = root / 'private/analysis/linux-module-set'
    data = json.loads((module_dir / 'manifest.json').read_text())
    if data['missing']:
        raise SystemExit('Missing module dependencies')
    modules = data['modules']
    ordered, active, done = [], set(), set()
    def visit(name):
        if name in done: return
        if name in active: raise ValueError('Module dependency cycle')
        active.add(name)
        for dep in modules[name]['deps']: visit(dep)
        active.remove(name); done.add(name); ordered.append(name)
    for name in modules: visit(name)
    # Refuse accidental persistent-storage or disk-logging modules in this artifact.
    forbidden = ('ufs', 'mmc', 'blackbox', 'mtdoops', 'charger_partition', 'bootmonitor')
    for name in ordered:
        if any(word in name for word in forbidden):
            raise SystemExit('Persistent storage/logging module excluded: ' + name)

    archive = bytearray()
    inode = 1
    def add(name, mode, payload=b'', major=0, minor=0):
        nonlocal inode
        if name.startswith('/') or '..' in Path(name).parts:
            raise ValueError('Invalid CPIO path')
        raw_name = name.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(payload), 0, 0, major, minor, len(raw_name), 0]
        archive.extend(b'070701' + ''.join(f'{x:08x}' for x in fields).encode())
        archive.extend(raw_name)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(payload)
        archive.extend(b'\0' * (-len(archive) % 4))
        inode += 1
    for directory in ('bin', 'sbin', 'lib', 'lib/modules', 'dev', 'proc', 'sys', 'tmp'):
        add(directory, stat.S_IFDIR | 0o755)
    add('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    add('dev/null', stat.S_IFCHR | 0o666, major=1, minor=3)
    add('bin/busybox', stat.S_IFREG | 0o755, busybox)
    for name in ('sh','mount','mknod','mkdir','sleep','echo','uname','insmod','ls','head','ln','seq','mdev','getty','reboot','cat','dmesg','lsmod'):
        add('bin/' + name, stat.S_IFLNK | 0o777, b'busybox')
    add('init', stat.S_IFREG | 0o755, (root / 'linux/initramfs/linux-ram/init').read_bytes())
    for name in ordered:
        info = modules[name]
        payload = (module_dir / info['file']).read_bytes()
        if hashlib.sha256(payload).hexdigest() != info['sha256']:
            raise SystemExit('Module capture hash mismatch: ' + name)
        add('lib/modules/' + info['file'], stat.S_IFREG | 0o644, payload)
    add('lib/module-order', stat.S_IFREG | 0o644, ('\n'.join(modules[n]['file'] for n in ordered) + '\n').encode())
    add('TRAILER!!!', 0)
    archive.extend(b'\0' * (-len(archive) % 512))
    out = root / 'artifacts/linux-ram'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'initramfs.cpio').write_bytes(archive)
    (out / 'initramfs.cpio.gz').write_bytes(gzip.compress(archive, mtime=0))
    result = {'status': 'HOST_ARTIFACT_ONLY_NOT_BOOTED', 'architecture': 'ARM64',
              'modules': len(ordered), 'cpio_bytes': len(archive),
              'sha256': hashlib.sha256(archive).hexdigest(),
              'persistent_filesystem_mounts': False, 'usb_function': 'ACM serial only',
              'recovery_timer_seconds': 180,
              'limitations': ['Artifact generation does not prove Linux boot',
                              'See per-test RAM logs for runtime loader/handoff verification',
                              'USB platform providers/firmware may require additional work']}
    (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
