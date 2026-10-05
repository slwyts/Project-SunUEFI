#!/usr/bin/env python3
"""Stream a complete GNU root into a reproducible, attribute-preserving archive.

Host-only; never accesses the tablet. Run through a private subordinate-ID user
namespace so package ownership is guest ownership, not the host backing IDs.
The archive is a Linux root payload, not a bootable firmware or a boot result.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 4 * 1024**3
DOWNLOAD_TARGET = 1024**3
# Empty mount points stay in the archive. Only their host/virtual contents go.
EMPTY = ('dev', 'proc', 'sys', 'run', 'tmp', 'sysroot', 'opt', 'mnt',
         'host-rootfs', 'var/cache/apt/archives', 'var/lib/apt/lists')
OMIT = ('host-rootfs', 'sysroot')


def sha_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024**2), b''):
            digest.update(chunk)
    return digest.hexdigest()


def excluded(name):
    if name in OMIT:
        return True
    if any(name.startswith(prefix + '/') for prefix in EMPTY):
        return True
    parts = name.split('/')
    # These two links refer to host build trees, never runtime module data.
    return len(parts) == 5 and parts[:3] == ['usr', 'lib', 'modules'] and parts[4] in ('build', 'source')


def snapshot(root):
    """No following symlinks or crossing a filesystem; preserve every feature."""
    root = Path(root)
    root_stat = root.lstat()
    if not stat.S_ISDIR(root_stat.st_mode):
        raise ValueError('Root must be a real directory')
    for name in ('opt', 'mnt', 'sysroot'):
        path = root / name
        if path.exists() or path.is_symlink():
            if path.is_symlink() or not path.is_dir():
                raise ValueError('Namespace mount point contains guest data: ' + name)
            with os.scandir(path) as entries:
                if next(entries, None) is not None:
                    raise ValueError('Namespace mount point contains guest data: ' + name)
    records, inode_names = [], {}
    logical, allocated = 0, 0

    def visit(path, name):
        nonlocal logical, allocated
        if excluded(name):
            return
        info = path.lstat()
        if info.st_dev != root_stat.st_dev:
            raise ValueError('Unexpected mount in root: ' + name)
        row = {'name': name, 'mode': info.st_mode, 'uid': info.st_uid,
               'gid': info.st_gid, 'size': 0}
        attributes = {}
        for key in sorted(os.listxattr(path, follow_symlinks=False)):
            attributes[key] = os.getxattr(path, key, follow_symlinks=False).hex()
        if attributes:
            row['xattrs'] = attributes
        if stat.S_ISDIR(info.st_mode):
            records.append(row)
            if name in EMPTY:
                return
            for entry in sorted(os.scandir(path), key=lambda item: item.name):
                visit(Path(entry.path), entry.name if name == '.' else name + '/' + entry.name)
        elif stat.S_ISREG(info.st_mode):
            key = (info.st_dev, info.st_ino)
            if key in inode_names:
                row['hardlink'] = inode_names[key]
            else:
                inode_names[key] = name
                row['size'] = info.st_size
                row['sha256'] = sha_file(path)
                logical += info.st_size
                # Conservative tmpfs allocation estimate, including small files.
                allocated += ((info.st_size + 4095) // 4096) * 4096
            records.append(row)
        elif stat.S_ISLNK(info.st_mode):
            row['symlink'] = os.readlink(path)
            records.append(row)
        else:
            raise ValueError('Unexpected special node in distro root: ' + name)

    visit(root, '.')
    fingerprint = hashlib.sha256(json.dumps(records, separators=(',', ':'), sort_keys=True).encode()).hexdigest()
    return {'tree_sha256': fingerprint, 'entries': len(records),
            'regular_logical_bytes': logical, 'regular_page_budget_bytes': allocated,
            'attribute_paths': [row['name'] for row in records if 'xattrs' in row],
            'records': records}


def tar_command(root):
    args = ['tar', '--create', '--format=posix', '--numeric-owner', '--sort=name',
            '--mtime=@0', '--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime',
            '--xattrs', '--xattrs-include=*', '--acls', '--one-file-system']
    args += ['--exclude=./' + name for name in OMIT]
    args += ['--exclude=./' + name + '/*' for name in EMPTY]
    args += ['--exclude=./usr/lib/modules/*/build', '--exclude=./usr/lib/modules/*/source']
    return args + ['--directory', str(root), '.']


def write_archive(root, archive):
    """Bound process output on disk, with neither a multi-GB Python buffer nor stdin code."""
    with Path(archive).open('xb') as target:
        producer = subprocess.Popen(tar_command(root), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        compressor = subprocess.Popen(['gzip', '-n', '-6'], stdin=producer.stdout, stdout=target, stderr=subprocess.PIPE)
        producer.stdout.close()
        try:
            _, error = compressor.communicate(timeout=1800)
            tar_error = producer.communicate(timeout=30)[1]
        except BaseException:
            compressor.kill(); producer.kill()
            compressor.wait(); producer.wait()
            raise
        if producer.returncode or compressor.returncode:
            raise ValueError('Archive pipeline failed: ' + (tar_error + error).decode(errors='replace'))
        target.flush(); os.fsync(target.fileno())


def verify_archive(archive, expected):
    """Read every compressed payload byte and compare every retained member."""
    wanted = {row['name']: row for row in expected['records']}
    seen = set(); bytes_checked = 0; xattr_count = 0
    with tarfile.open(archive, 'r|gz') as stream:
        for item in stream:
            name = item.name
            while name.startswith('./'):
                name = name[2:]
            name = name.rstrip('/') or '.'
            if name in seen or name not in wanted:
                raise ValueError('Duplicate or unexpected payload member: ' + name)
            seen.add(name); row = wanted[name]; mode = row['mode']
            if (item.uid, item.gid, item.mode & 0o7777) != (row['uid'], row['gid'], mode & 0o7777):
                raise ValueError('Payload ownership or permissions differ: ' + name)
            if stat.S_ISDIR(mode):
                valid = item.isdir()
            elif stat.S_ISLNK(mode):
                valid = item.issym() and item.linkname == row['symlink']
            elif 'hardlink' in row:
                valid = item.islnk() and item.linkname.removeprefix('./') == row['hardlink']
            else:
                valid = item.isfile() and item.size == row['size']
                if valid:
                    digest = hashlib.sha256()
                    source = stream.extractfile(item)
                    for chunk in iter(lambda: source.read(1024**2), b''):
                        digest.update(chunk); bytes_checked += len(chunk)
                    valid = digest.hexdigest() == row['sha256']
            if not valid:
                raise ValueError('Payload content or link differs: ' + name)
            for key, value in row.get('xattrs', {}).items():
                if 'hardlink' in row:
                    first = wanted[row['hardlink']]
                    if row['hardlink'] not in seen or first.get('xattrs', {}).get(key) != value:
                        raise ValueError('Hardlink attributes differ from the checked inode: ' + name)
                    xattr_count += 1
                    continue
                if key.startswith('system.posix_acl_'):
                    # GNU tar serializes ACLs to textual PAX, tested by its
                    # actual extraction; raw ACL xattrs need not be duplicated.
                    pax_key = 'SCHILY.acl.' + key.removeprefix('system.posix_acl_')
                    if pax_key not in item.pax_headers:
                        raise ValueError('Payload ACL missing: ' + name)
                else:
                    raw = item.pax_headers.get('SCHILY.xattr.' + key)
                    if raw is None or raw.encode('utf-8', 'surrogateescape').hex() != value:
                        raise ValueError('Payload xattr differs: ' + name + ' ' + key)
                xattr_count += 1
    if seen != set(wanted):
        raise ValueError('Payload omitted retained root members')
    return {'all_members_checked': len(seen), 'regular_bytes_sha256_checked': bytes_checked,
            'attributes_checked': xattr_count, 'guest_ownership_checked': True}


def package(root, output):
    root, output = Path(root).resolve(), Path(output).resolve()
    if not root.is_relative_to(ROOT / 'build/distros') or not root.is_dir():
        raise ValueError('Only an explicit derived workspace distro is accepted')
    if not output.is_relative_to(ROOT / 'artifacts') or output.exists():
        raise ValueError('Output must be a fresh artifact directory')
    before = snapshot(root)
    if before['regular_page_budget_bytes'] >= LIMIT:
        raise ValueError('Unpacked root exceeds the 4GiB tmpfs budget')
    # Refuse a mismatched staged entry instead of packaging an obsolete policy.
    if (root / 'pianoinit').read_bytes() != (ROOT / 'bootprofiles/linux-userspace/pianoinit').read_bytes():
        raise ValueError('Staged pianoinit differs from current RAM entry')
    if not (root / 'usr/lib/systemd/systemd').is_file():
        raise ValueError('Complete GNU systemd root is required')
    output.mkdir(parents=True)
    archive = output / 'Piano-rootfs.tar.gz'
    write_archive(root, archive)
    verification = verify_archive(archive, before)
    after = snapshot(root)
    if before != after:
        raise ValueError('Root changed during packaging; artifact is not accepted')
    result = {key: value for key, value in before.items() if key != 'records'}
    result.update({'status': 'HOST_PACKAGED_GNU_ROOT_NOT_BOOT_VERIFIED',
                   'root_policy': 'RAM_ONLY_NO_ANDROID_BLOCK_MOUNT',
                   'root_limit_bytes': LIMIT, 'download_target_bytes': DOWNLOAD_TARGET,
                   'archive_bytes': archive.stat().st_size, 'archive_sha256': sha_file(archive),
                   'archive_verification': verification,
                   'fits_1gib_root_payload': archive.stat().st_size <= DOWNLOAD_TARGET,
                   'attribute_preservation': 'GNU_TAR_POSIX_PAX_XATTRS_ACLS_NUMERIC_GUEST_OWNERS',
                   'package_tool_sha256': sha_file(__file__), 'piano_boot_verified': False,
                   'firmware_download_limit_changed': False})
    (output / 'tree-manifest.json').write_text(json.dumps(before, indent=2) + '\n')
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--private-userns', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.private_userns:
        # Root maps to the current owner; auto maps this user's subordinate IDs.
        os.execvp('unshare', ['unshare', '--user', '--map-root-user', '--map-auto', '--fork',
                             sys.executable, str(Path(__file__).resolve()),
                             *sys.argv[1:], '--private-userns'])
    if os.getuid() or os.getgid():
        raise ValueError('Expected guest root inside a private mapped user namespace')
    print(json.dumps(package(args.rootfs, args.output), indent=2))


if __name__ == '__main__':
    main()
