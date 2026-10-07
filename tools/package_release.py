#!/usr/bin/env python3
"""Build an offline Piano bundle with real FAT/ext4 tools; never access a device."""
import argparse
from collections import Counter
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

from make_kernel_initramfs import inspect_newc
from build_piano_ram_bootstrap import guest_resolve
from prepare_boot_files import fdt_info, image_info, initramfs_info

ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True, text=True, **kwargs)


def record(path):
    data = json.loads(path.read_text()) if path.is_file() else {}
    require(isinstance(data, dict), 'Expected manifest object: ' + str(path))
    return data


def component(path, metadata, explicit, kind):
    require(path.is_file() and not path.is_symlink(), 'Expected regular component: ' + str(path))
    expected = metadata.get('files', {}).get(path.name, {}).get('sha256')
    entry = metadata.get(kind, {})
    expected = expected or (entry.get('sha256') if isinstance(entry, dict) else None) or metadata.get(kind + '_sha256')
    if explicit and expected:
        require(explicit == expected, 'Explicit hash disagrees with manifest: ' + path.name)
    expected = explicit or expected
    require(isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected), 'Missing component hash: ' + path.name)
    require(sha(path) == expected, 'Component hash mismatch: ' + path.name)
    return expected


def scan_root(root, release):
    """Inspect names/stat only for credentials; never open their contents."""
    require(root.is_dir() and not root.is_symlink(), 'Expected a clean rootfs directory')
    rows, links = {}, Counter()
    for path in [root, *sorted(root.rglob('*'))]:
        name = path.relative_to(root).as_posix()
        require(not any(c in name for c in '\n\r"\\'), 'Unsupported rootfs filename')
        info = path.lstat()
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode), 'Special rootfs file: ' + name)
        if name.split('/')[0] in ('dev', 'proc', 'sys', 'run'):
            require(name in ('dev', 'proc', 'sys', 'run') and stat.S_ISDIR(info.st_mode), 'Runtime tree is not empty: ' + name)
        secret = '.ssh' in path.relative_to(root).parts or name.startswith('etc/ssh/ssh_host_')
        machine = name in ('etc/machine-id', 'var/lib/dbus/machine-id')
        identity = guest_resolve(root, '/' + name).lstat() if machine and stat.S_ISLNK(info.st_mode) else info
        require(not (secret or machine) or (stat.S_ISREG(identity.st_mode) and identity.st_size == 0) or stat.S_ISDIR(identity.st_mode),
                'Nonempty identity/SSH material: ' + name)
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            parts = [] if target.startswith('/') else list(path.relative_to(root).parent.parts)
            for part in PurePosixPath(target).parts:
                if part in ('/', '.'):
                    continue
                if part == '..':
                    require(parts, 'Symlink escapes rootfs: ' + name)
                    parts.pop()
                else:
                    parts.append(part)
            require(parts and parts[0] in ('bin', 'sbin', 'lib', 'lib64', 'usr', 'etc', 'var', 'run', 'proc', 'sys', 'dev', 'root', 'home', 'opt', 'tmp', 'boot'),
                    'Host symlink in rootfs: ' + name)
            mapped = guest_resolve(root, '/' + name)
            require(not target.startswith('/') or mapped.exists() or parts[0] in ('run', 'proc', 'sys', 'dev'), 'Absolute host/dangling symlink: ' + name)
        if stat.S_ISREG(info.st_mode):
            links[(info.st_dev, info.st_ino)] += 1
        rows[name] = info
    for name, info in rows.items():
        if stat.S_ISREG(info.st_mode):
            require(links[(info.st_dev, info.st_ino)] == info.st_nlink, 'Hardlink outside rootfs: ' + name)
    modules = guest_resolve(root, '/lib/modules')
    if not modules.is_dir():
        modules = guest_resolve(root, '/usr/lib/modules')
    require(modules.is_dir() and sorted(p.name for p in modules.iterdir()) == [release], 'Rootfs kernel modules/release mismatch')
    return rows


def pinned_mkbootimg():
    pins = record(ROOT / 'sources.lock.json')
    for name, relative in (('debian-piano-current', 'mkbootimg/mkbootimg.py'), ('Mu-Silicium', 'Resources/Scripts/mkbootimg.py')):
        checkout = ROOT / 'upstream' / name
        script = checkout / relative
        if not script.is_file():
            continue
        pin = pins[name]['commit']
        registered = run(['git', '-C', ROOT, 'ls-files', '--stage', '--', checkout.relative_to(ROOT)]).stdout.split()
        require(len(registered) >= 2 and registered[0] == '160000' and registered[1] == pin, 'mkbootimg source is not the registered pin')
        require(run(['git', '-C', checkout, 'rev-parse', 'HEAD']).stdout.strip() == pin, 'mkbootimg checkout differs from source pin')
        folder = str(Path(relative).parent)
        tracked = run(['git', '-C', checkout, 'ls-tree', '-r', '--name-only', pin, '--', folder]).stdout.splitlines()
        for item in tracked:
            if item.endswith('.py'):
                original = subprocess.check_output(['git', '-C', str(checkout), 'show', pin + ':' + item])
                require((checkout / item).read_bytes() == original, 'Modified mkbootimg Python source: ' + item)
        require(not run(['git', '-C', checkout, 'status', '--porcelain', '--untracked-files=all', '--', folder]).stdout.strip(), 'Dirty mkbootimg source directory')
        return script, {'source': name, 'commit': pin, 'script_sha256': sha(script)}
    raise ValueError('No pinned local mkbootimg source; initialize the registered submodule offline first')


def verify_boot(path, inputs):
    data = path.read_bytes()
    require(len(data) >= 1660 and data[:8] == b'ANDROID!', 'Invalid boot container')
    ksize, _, rsize, _, second, _, _, page, version = struct.unpack_from('<9I', data, 8)
    recovery = struct.unpack_from('<I', data, 1632)[0]
    header, dsize = struct.unpack_from('<II', data, 1644)
    require((page, version, header, second, recovery) == (4096, 2, 1660, 0, 0), 'Expected Android v2/4096 without extra stages')
    require(not any(data[64:576]) and not any(data[608:1632]), 'Unexpected wrapper command line')
    offset = page
    for size, source in zip((ksize, rsize, dsize), inputs):
        require(size == source.stat().st_size and data[offset:offset + size] == source.read_bytes(), 'Boot wrapper component mismatch')
        offset += (size + page - 1) // page * page
    require(len(data) == offset, 'Unexpected boot wrapper trailing data')


def archive_root(root, target, epoch):
    with tempfile.TemporaryFile() as errors:
        tar = subprocess.Popen(['tar', '--numeric-owner', '--xattrs', '--acls', '--sort=name', '--format=pax',
                                '--pax-option=delete=atime,delete=ctime', '--mtime=@' + str(epoch), '-C', str(root), '-cf', '-', '.'],
                               stdout=subprocess.PIPE, stderr=errors)
        try:
            subprocess.run(['zstd', '-q', '-T1', '-19', '-o', str(target)], stdin=tar.stdout, check=True)
            tar.stdout.close()
            require(tar.wait() == 0, 'tar failed; rootfs may have changed during packing')
        finally:
            if tar.poll() is None:
                tar.terminate()
                tar.wait()


def quote(name):
    return '"' + ('/' if name == '.' else '/' + name) + '"'


def xattr_equal(name, left, right):
    if name in ('system.posix_acl_access', 'system.posix_acl_default'):
        # POSIX ACL object/mask entries have no numeric identity. libext2fs
        # emits zero there, while the kernel exposes ACL_UNDEFINED_ID.
        def acl(data):
            require(len(data) >= 4 and (len(data) - 4) % 8 == 0 and data[:4] == b'\2\0\0\0', 'Invalid POSIX ACL format')
            return [(tag, perm, ident if tag in (2, 8) else None) for tag, perm, ident in struct.iter_unpack('<HHI', data[4:])]
        return acl(left) == acl(right)
    return left == right


def ext4(root, target, size, label, epoch, rows, work):
    """No mount: verify every inode's mode/owner/mtime and source xattrs."""
    with target.open('xb') as stream:
        stream.truncate(size * MIB)
    owner = rows['.']
    run(['mke2fs', '-q', '-t', 'ext4', '-F', '-L', label, '-E', f'root_owner={owner.st_uid}:{owner.st_gid}', '-d', root, target], env={**os.environ, 'E2FSPROGS_FAKE_TIME': str(epoch)})
    script = work / 'inode-checks'
    script.write_text(f'set_inode_field "/" mode 0{owner.st_mode:o}\n' + ''.join(f'set_inode_field {quote(n)} mtime {epoch}\nset_inode_field {quote(n)} ctime {epoch}\nstat {quote(n)}\n' for n in rows))
    output = run(['debugfs', '-w', '-f', script, target]).stdout
    sections = re.split(r'debugfs:\s+stat ', output)[1:]
    require(len(sections) == len(rows), 'ext4 inode checks incomplete')
    for (name, info), text in zip(rows.items(), sections):
        mode = re.search(r'Mode:\s+([0-7]+)', text)
        owner = re.search(r'User:\s*(\d+)\s+Group:\s*(\d+)', text)
        mtime = re.search(r'mtime:\s+0x([0-9a-f]+)', text)
        ctime = re.search(r'ctime:\s+0x([0-9a-f]+)', text)
        require(mode and owner and mtime and int(mode[1], 8) == stat.S_IMODE(info.st_mode)
                and ctime and tuple(map(int, owner.groups())) == (info.st_uid, info.st_gid)
                and int(mtime[1], 16) == int(ctime[1], 16) == epoch,
                'ext4 metadata mismatch: ' + name)
        for attr in os.listxattr(root / name, follow_symlinks=False):
            require(re.fullmatch('[A-Za-z0-9_.-]+', attr), 'Unsupported xattr name')
            saved = work / 'xattr-value'
            saved.unlink(missing_ok=True)
            run(['debugfs', '-R', f'ea_get -f "{saved}" {quote(name)} {attr}', target])
            require(saved.is_file() and xattr_equal(attr, saved.read_bytes(), os.getxattr(root / name, attr, follow_symlinks=False)), 'ext4 xattr/ACL mismatch: ' + name)


def package(args):
    uefi, kernel, dtb, initrd, root = [Path(getattr(args, n)).absolute() for n in ('uefi', 'kernel', 'dtb', 'initramfs', 'rootfs')]
    require(not root.is_symlink(), 'Rootfs must not be a host symlink')
    require(not args.output.is_symlink(), 'Output must not be a host symlink')
    root, output = root.resolve(), args.output.resolve()
    require(not output.exists() and not output.is_relative_to(root), 'Output must be fresh and outside rootfs')
    require(64 <= args.esp_size_mib <= 2048 and (args.root_size_mib is None or 64 <= args.root_size_mib <= 16384), 'Invalid image capacity; expand the dedicated root partition during installation')
    require(0 <= args.epoch <= 0x7fffffff, 'Invalid SOURCE_DATE_EPOCH')
    require(re.fullmatch('LABEL=[A-Za-z0-9_-]{1,16}', args.root_selector), 'Portable bundle requires a filesystem LABEL root selector')
    metas = [record(uefi.parent / 'manifest.json'), record(kernel / 'manifest.json'), record(dtb.parent / 'manifest.json'), record(initrd.parent / 'manifest.json')]
    image = kernel / 'Image'
    hashes = [component(p, m, getattr(args, key + '_sha256'), kind) for p, m, key, kind in zip((uefi, image, dtb, initrd), metas,
              ('uefi', 'kernel', 'dtb', 'initramfs'), ('uefi', 'image', 'output', 'initramfs'))]
    require(metas[1].get('source_dirty') is not True and metas[1].get('source_clean') is not False
            and not any(word in metas[1].get('status', '').upper() for word in ('FAILED', 'INCOMPLETE')), 'Kernel manifest is not a completed clean build')
    release = args.kernel_release or metas[1].get('kernel_release')
    require(isinstance(release, str) and re.fullmatch('[A-Za-z0-9._+-]{1,128}', release), 'Missing/invalid kernel release')
    for meta in metas[1:]:
        require(not meta.get('kernel_release') or meta['kernel_release'] == release, 'Component kernel release mismatch')
    require(args.rootfs_manifest is None or args.rootfs_manifest.is_file(), 'Rootfs manifest does not exist')
    root_meta = record(args.rootfs_manifest or root.parent / 'manifest.json')
    require(not root_meta.get('kernel_release') or root_meta['kernel_release'] == release, 'Rootfs manifest release mismatch')
    for meta in [*metas[1:], root_meta]:
        policy = meta.get('root_policy') or meta.get('root_selector')
        require(not policy or policy == args.root_selector, 'Component root selector mismatch')
    for meta in (metas[3], root_meta):
        if meta.get('kernel_manifest_sha256'):
            require(sha(kernel / 'manifest.json') == meta['kernel_manifest_sha256'], 'Component refers to another kernel manifest')
        if meta.get('kernel_commit'):
            require(meta['kernel_commit'] == metas[1].get('source_commit'), 'Rootfs kernel commit mismatch')
    command_line = metas[1].get('command_line', '').split()
    roots = [v.split('=', 1)[1] for v in command_line if v.startswith(('root=', 'piano.root='))]
    require(not roots or roots == [args.root_selector], 'Kernel command line selects another root')
    if metas[1].get('config_sha256'):
        require(sha(kernel / 'config') == metas[1]['config_sha256'], 'Kernel config hash mismatch')
    require(uefi.read_bytes()[:8] == b'ANDROID!', 'UEFI must be an Android boot container')
    image_info(image)
    fdt_info(dtb)
    initramfs_info(initrd)
    with gzip.open(initrd, 'rb') as stream:
        ram = stream.read(128 * MIB + 1)
    require(len(ram) <= 128 * MIB, 'Initramfs exceeds the bounded bootstrap budget')
    entries = inspect_newc(ram)
    embedded = {r['name']: r['data'] for r in entries}
    require('init' in embedded or 'pianoinit' in embedded, 'Initramfs has no init')
    if 'etc/piano/kernel-release' in embedded:
        require(embedded['etc/piano/kernel-release'].decode().strip() == release, 'Initramfs embedded release mismatch')
    require(all(n.split('/')[2] == release for n in embedded if n.startswith('lib/modules/') and len(n.split('/')) > 2), 'Initramfs module release mismatch')
    rows = scan_root(root, release)
    tools = ['tar', 'zstd', 'mkfs.vfat', 'mmd', 'mcopy'] + (['mke2fs', 'debugfs'] if args.root_size_mib else [])
    require(all(shutil.which(t) for t in tools), 'Missing host tools: ' + ', '.join(t for t in tools if not shutil.which(t)))
    maker, maker_source = pinned_mkbootimg()
    output.mkdir(parents=True)
    incomplete = output / '.incomplete'
    incomplete.write_text('Packaging has not completed.\n')
    work = output / '.work'
    work.mkdir()
    sources = (uefi, image, dtb, initrd)
    names = ('PianoUEFI-product.img', 'Image', 'board.dtb', 'initramfs')
    for source, name, expected in zip(sources, names, hashes):
        shutil.copyfile(source, work / name)
        require(sha(work / name) == expected, 'Source changed while copying: ' + name)
    boot = output / 'boot.img'
    run([sys.executable, maker, '--header_version', '2', '--pagesize', '4096', '--kernel', work / 'Image', '--ramdisk', work / 'initramfs', '--dtb', work / 'board.dtb', '-o', boot],
        env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
    verify_boot(boot, (work / 'Image', work / 'initramfs', work / 'board.dtb'))
    for path in (work / 'Image', work / 'board.dtb', work / 'initramfs', boot):
        os.utime(path, (max(args.epoch, 315532800),) * 2)  # FAT dates start in 1980.
    esp = output / 'esp.img'
    require(sum((work / n).stat().st_size for n in names[1:]) + boot.stat().st_size + 2 * MIB < args.esp_size_mib * MIB, 'Payload exceeds ESP capacity')
    with esp.open('xb') as stream:
        stream.truncate(args.esp_size_mib * MIB)
    run(['mkfs.vfat', '--invariant', '-F', '32', '-n', 'SUNUEFI_ESP', esp])
    run(['mmd', '-i', esp, '::/EFI', '::/EFI/Piano', '::/EFI/Piano/stable'])
    for path in (work / 'Image', work / 'board.dtb', work / 'initramfs', boot):
        run(['mcopy', '-m', '-i', esp, path, '::/EFI/Piano/stable/' + path.name])
        verify = work / ('readback-' + path.name)
        run(['mcopy', '-i', esp, '::/EFI/Piano/stable/' + path.name, verify])
        require(sha(verify) == sha(path), 'ESP file readback differs: ' + path.name)
    archive_root(root, output / 'root.tar.zst', args.epoch)
    if args.root_size_mib:
        ext4(root, output / 'root.ext4.img', args.root_size_mib, args.root_selector.split('=', 1)[1], args.epoch, rows, work)
    require({n: (s.st_mode, s.st_uid, s.st_gid, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for n, s in rows.items()} ==
            {n: (s.st_mode, s.st_uid, s.st_gid, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for n, s in scan_root(root, release).items()}, 'Rootfs changed while packaging')
    shutil.move(work / names[0], output / names[0])
    result = {'schema_version': 1, 'status': 'HOST_BUILT_NOT_DEVICE_READY', 'kernel_release': release, 'root_policy': args.root_selector,
              'epoch': args.epoch, 'mkbootimg': maker_source, 'device_operation_performed': False, 'device_ready': False,
              'components': {name: {'sha256': value} for name, value in zip(('uefi', 'kernel', 'dtb', 'initramfs'), hashes)},
              'kernel_manifest_sha256': sha(kernel / 'manifest.json') if (kernel / 'manifest.json').is_file() else None,
              'partitions': {'esp': {'partlabel': 'sunuefi_esp', 'label': 'SUNUEFI_ESP', 'capacity_bytes': esp.stat().st_size},
                             'root': {'partlabel': 'sunuefi_root', 'label': args.root_selector.split('=', 1)[1], 'capacity_bytes': args.root_size_mib * MIB if args.root_size_mib else None}},
              'esp_files': ['/EFI/Piano/stable/' + n for n in ('Image', 'board.dtb', 'initramfs', 'boot.img')],
              'metadata': {'archive': 'numeric uid/gid, modes, symlinks, hardlinks, xattrs and ACLs; mtime normalized',
                           'ext4': 'all inode uid/gid/mode/mtime and source xattrs/ACLs checked; ctime normalized; sparse source allocation not preserved'},
              'files': {}}
    shutil.rmtree(work)
    for path in sorted(output.iterdir()):
        if path.name == '.incomplete':
            continue
        result['files'][path.name] = {'bytes': path.stat().st_size, 'sha256': sha(path), 'format': 'raw-fat32' if path == esp else 'raw-ext4' if path.name == 'root.ext4.img' else 'tar-zstd' if path.name == 'root.tar.zst' else 'android-boot', 'expanded_bytes': path.stat().st_size}
    manifest = output / 'manifest.json'
    manifest.write_text(json.dumps(result, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in sorted(output.iterdir()) if p.name not in ('.incomplete', 'SHA256SUMS')))
    incomplete.unlink()
    return result


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('uefi', 'kernel', 'dtb', 'initramfs', 'rootfs', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
    for name in ('uefi', 'kernel', 'dtb', 'initramfs'):
        ap.add_argument('--' + name + '-sha256')
    ap.add_argument('--kernel-release')
    ap.add_argument('--rootfs-manifest', type=Path)
    ap.add_argument('--root-selector', default='LABEL=PIANOROOT')
    ap.add_argument('--esp-size-mib', type=int, default=512)
    ap.add_argument('--root-size-mib', type=int)
    ap.add_argument('--epoch', type=int, default=int(os.environ.get('SOURCE_DATE_EPOCH', '0')))
    return ap


if __name__ == '__main__':
    try:
        manifest = package(parser().parse_args())
        print(json.dumps({'status': manifest['status'], 'files': manifest['files']}, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
