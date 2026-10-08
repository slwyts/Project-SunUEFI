#!/usr/bin/env python3
"""Derive a per-device ext4 root with an explicitly supplied SSH public key.

The generic image stays unchanged. Linux uses e2fsprogs/coreutils; Windows uses
the same tools in WSL. No mount, private key, password, sudo or host-key change.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


def require(ok, message):
    if not ok:
        raise ValueError(message)


def file_sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def public_key(path):
    """Accept one ordinary OpenSSH public key; discard its optional comment."""
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= 16384,
            'Expected a regular OpenSSH public-key file, at most 16 KiB')
    with path.open('rb') as stream:
        # A private-key header ends at its newline: never read its body.
        prefix = stream.readline(64)
        tokens = prefix.split(None, 1)
        kind = tokens[0] if tokens else b''
        require(kind in (b'ssh-ed25519', b'ssh-rsa', b'ecdsa-sha2-nistp256',
                         b'ecdsa-sha2-nistp384', b'ecdsa-sha2-nistp521'),
                'Supply a .pub file containing an OpenSSH public key, never a private key')
        data = prefix + stream.read(16385 - len(prefix))
    lines = data.splitlines()
    require(len(lines) == 1, 'Supply exactly one public key, without authorized_keys options')
    fields = lines[0].split(None, 2)
    require(len(fields) >= 2 and fields[0] == kind, 'Invalid public-key text')
    try:
        blob = base64.b64decode(fields[1], validate=True)
    except ValueError as error:
        raise ValueError('Invalid public-key base64') from error
    pos = 0
    def word():
        nonlocal pos
        require(pos + 4 <= len(blob), 'Truncated public-key structure')
        length = struct.unpack_from('>I', blob, pos)[0];pos += 4
        require(length <= len(blob) - pos, 'Truncated public-key field')
        value = blob[pos:pos + length];pos += length
        return value
    require(word() == kind, 'Public-key type differs from its encoded structure')
    if kind == b'ssh-ed25519':
        require(len(word()) == 32, 'Invalid Ed25519 public-key length')
    elif kind == b'ssh-rsa':
        require(bool(word()) and bool(word()), 'Invalid RSA public-key fields')
    else:
        curve = kind.removeprefix(b'ecdsa-sha2-')
        require(word() == curve, 'Invalid ECDSA curve')
        point = word()
        width = {b'nistp256': 32, b'nistp384': 48, b'nistp521': 66}[curve]
        require(len(point) == 1 + 2 * width and point[0] == 4, 'Invalid ECDSA public point')
    require(pos == len(blob), 'Unexpected extra public-key fields')
    normalized = kind + b' ' + base64.b64encode(blob) + b'\n'
    return normalized, {'type': kind.decode('ascii'),
                        'fingerprint': 'SHA256:' + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip('=')}


class HostTools:
    def __init__(self):
        self.windows = os.name == 'nt'
        self.paths = {}
        if self.windows:
            require(shutil.which('wsl.exe'), 'SSH image provisioning on Windows requires WSL with e2fsprogs and coreutils')
            r = self.run(['sh', '-c', 'command -v debugfs && command -v e2fsck && command -v cp'])
            require(r.returncode == 0, 'Install e2fsprogs and coreutils in the default WSL distribution')
        else:
            require(all(shutil.which(name) for name in ('debugfs', 'e2fsck', 'cp')),
                    'SSH image provisioning requires e2fsprogs (debugfs/e2fsck) and coreutils (cp)')

    def path(self, path):
        path = str(Path(path).absolute())
        require('\n' not in path and '\r' not in path, 'Image paths must not contain newlines')
        if self.windows and path not in self.paths:
            r = subprocess.run(['wsl.exe', '--exec', 'wslpath', '-a', '-u', path], capture_output=True, text=True, encoding='utf-8')
            require(r.returncode == 0 and r.stdout.strip().startswith('/') and '\n' not in r.stdout.strip(),
                    'Cannot translate the image path into WSL')
            self.paths[path] = r.stdout.strip()
        return self.paths[path] if self.windows else path

    def run(self, argv):
        args = [self.path(value) if isinstance(value, Path) else str(value) for value in argv]
        if self.windows:
            args = ['wsl.exe', '--exec', 'env', 'LC_ALL=C', 'LANG=C', *args]
        return subprocess.run(args, capture_output=True, text=True, encoding='utf-8',
                              env={**os.environ, 'LC_ALL': 'C', 'LANG': 'C'})


def quote(value):
    require('\n' not in value and '\r' not in value, 'Invalid debugfs path')
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def debug(tools, image, command, write=False):
    r = tools.run(['debugfs', *(['-w'] if write else []), '-R', command, image])
    require(r.returncode == 0, 'debugfs could not process the root image')
    return r.stdout, r.stderr


def inode(tools, image, path):
    out, err = debug(tools, image, 'stat ' + quote(path))
    match = re.search(r'Inode:\s+\d+\s+Type:\s+(\w+)\s+Mode:\s+([0-7]+)', out)
    if not match:
        require('File not found' in err, 'Could not inspect ext4 inode: ' + path)
        return None
    owner = re.search(r'User:\s*(\d+)\s+Group:\s*(\d+)', out)
    require(owner, 'Missing ext4 inode owner: ' + path)
    return {'type': match[1], 'mode': int(match[2], 8),
            'uid': int(owner[1]), 'gid': int(owner[2])}


def read_file(tools, image, path, output, limit):
    info = inode(tools, image, path)
    require(info and info['type'] == 'regular', 'Expected a regular image file: ' + path)
    debug(tools, image, 'dump ' + quote(path) + ' ' + quote(tools.path(output)))
    require(output.is_file() and output.stat().st_size <= limit, 'Missing/oversized image file: ' + path)
    return output.read_bytes()


def set_metadata(tools, image, path, mode, uid, gid):
    for field, value in (('mode', '0' + format(mode, 'o')), ('uid', str(uid)), ('gid', str(gid))):
        debug(tools, image, 'set_inode_field ' + quote(path) + ' ' + field + ' ' + value, True)


def provision_image(image, output, key_file, expected_sha256=None):
    image, output = Path(image).absolute(), Path(output).absolute()
    key, key_info = public_key(key_file)
    require(image.is_file() and not image.is_symlink(), 'Expected a regular generic ext4 image')
    require(not output.exists() and not output.is_symlink() and output.parent.is_dir(),
            'Derived root must be a new file in an existing directory')
    with image.open('rb') as stream:
        stream.seek(1080)
        require(stream.read(2) == b'\x53\xef', 'Expected a raw ext4 image')
    source_sha = file_sha(image)
    require(expected_sha256 is None or source_sha == expected_sha256, 'Generic root SHA256 differs')
    tools = HostTools()
    made_output = False
    try:
        r = tools.run(['cp', '--reflink=auto', '--sparse=always', '--', image, output])
        made_output = output.exists()
        require(r.returncode == 0 and made_output and file_sha(output) == source_sha, 'Could not copy the generic root image')
        r = tools.run(['e2fsck', '-f', '-n', output])
        require(r.returncode == 0, 'Generic root filesystem is not clean; refusing SSH provisioning')
        with tempfile.TemporaryDirectory(prefix='piano-ssh-public-', dir=output.parent) as folder:
            work = Path(folder)
            passwd = read_file(tools, output, '/etc/passwd', work / 'passwd', 1024 * 1024)
            accounts = {}
            for line in passwd.decode().splitlines():
                values = line.split(':')
                if len(values) == 7 and values[0] in ('root', 'piano'):
                    require(values[0] not in accounts, 'Duplicate target account in image')
                    accounts[values[0]] = (int(values[2]), int(values[3]), values[5])
            require(accounts.get('root') == (0, 0, '/root') and 'piano' in accounts
                    and accounts['piano'][0] > 0 and accounts['piano'][2] == '/home/piano',
                    'Image must contain the standard root and piano accounts')
            key_path = work / 'authorized_keys';key_path.write_bytes(key)
            verified = []
            for name in ('root', 'piano'):
                uid, gid, home = accounts[name]
                parent = inode(tools, output, home)
                require(parent and parent['type'] == 'directory' and (parent['uid'], parent['gid']) == (uid, gid),
                        'Target home ownership/type differs: ' + home)
                directory, target = home + '/.ssh', home + '/.ssh/authorized_keys'
                existing = inode(tools, output, directory)
                require(existing is None or existing['type'] == 'directory', 'SSH directory must not be a symlink or other file')
                if existing is None:
                    debug(tools, output, 'mkdir ' + quote(directory), True)
                old_key = inode(tools, output, target)
                if old_key:
                    old = read_file(tools, output, target, work / ('old-' + name), 16384)
                    require(not old.strip(), 'Generic image already contains SSH authorization; refusing to replace it')
                    debug(tools, output, 'rm ' + quote(target), True)
                debug(tools, output, 'write ' + quote(tools.path(key_path)) + ' ' + quote(target), True)
                set_metadata(tools, output, directory, 0o40700, uid, gid)
                set_metadata(tools, output, target, 0o100600, uid, gid)
                require(inode(tools, output, directory) == {'type': 'directory', 'mode': 0o700, 'uid': uid, 'gid': gid},
                        'SSH directory metadata check failed')
                require(inode(tools, output, target) == {'type': 'regular', 'mode': 0o600, 'uid': uid, 'gid': gid}
                        and read_file(tools, output, target, work / ('check-' + name), 16384) == key,
                        'Authorized-key content/metadata check failed')
                verified.append({'account': name, 'uid': uid, 'gid': gid, 'authorized_keys': target,
                                 'directory_mode': '0700', 'file_mode': '0600'})
        r = tools.run(['e2fsck', '-f', '-n', output])
        require(r.returncode == 0, 'Derived root filesystem check failed')
        require(image.stat().st_size == output.stat().st_size and file_sha(image) == source_sha,
                'Generic root changed during provisioning')
        return {'schema_version': 1, 'status': 'HOST_DERIVED_SSH_ROOT_NOT_BOOT_VERIFIED',
                'source_sha256': source_sha, 'derived_sha256': file_sha(output),
                'bytes': output.stat().st_size, 'public_key': key_info, 'accounts': verified,
                'generic_image_changed': False, 'private_key_accessed': False,
                'password_or_sudo_changed': False, 'host_keys_changed': False,
                'backend': 'WSL e2fsprogs' if tools.windows else 'native e2fsprogs'}
    except Exception:
        if made_output:
            output.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path, help='Unmodified generic raw ext4 root image')
    parser.add_argument('--output', required=True, type=Path, help='New per-device derived ext4 file')
    parser.add_argument('--ssh-public-key', required=True, type=Path, help='One OpenSSH .pub key; private keys are never accepted')
    parser.add_argument('--expected-source-sha256', help='Optional manifest-bound generic image SHA256')
    args = parser.parse_args()
    try:
        print(json.dumps(provision_image(args.image, args.output, args.ssh_public_key, args.expected_source_sha256), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
