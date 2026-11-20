"""Remove the former image-copied fingerprint tool, not local replacements."""
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess


RECEIPT = 'usr/share/piano-provenance/runtime-helpers.json'
LEGACY = {
    'usr/local/sbin/piano-fingerprint-ctl': 0o755,
    'usr/share/piano-provenance/fingerprint.json': 0o644,
    **{f'usr/share/licenses/piano-fingerprint/{name}': 0o644
       for name in ('BSD-3-Clause.txt', 'BSD-3-Clause-Clear.txt', 'qcbor.txt', 'linux-tee.h')},
}


def regular_file(root, name, mode):
    path = root
    parts = Path(name).parts
    try:
        for part in parts[:-1]:
            path /= part
            directory = path.lstat()
            if (not stat.S_ISDIR(directory.st_mode) or directory.st_uid != 0 or
                    directory.st_mode & 0o022):
                return None
        path /= parts[-1]
        info = path.lstat()
    except OSError:
        return None
    if (stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) == mode and
            (info.st_uid, info.st_gid, info.st_nlink) == (0, 0, 1)):
        return path, info
    return None


def remove_legacy(root, unowned):
    """Use the old image's receipt so different release builds are recognised."""
    if not (receipt := regular_file(root, RECEIPT, 0o644)):
        return
    try:
        record = json.loads(receipt[0].read_text())
        if (record['status'] != 'REAL_RUNTIME_STAGED_NOT_DEVICE_TESTED' or
                not re.fullmatch(r'[a-f0-9]{64}', record['bundle_manifest_sha256'])):
            return
        files = record['files']
    except (OSError, ValueError, KeyError, TypeError):
        return
    if not isinstance(files, dict):
        return
    for name, mode in LEGACY.items():
        row = files.get(name)
        if (not isinstance(row, dict) or row.get('file') != 'fingerprint/' + Path(name).name or
                row.get('mode') != mode or not (existing := regular_file(root, name, mode))):
            continue
        path, info = existing
        if row.get('bytes') != info.st_size:
            continue
        try:
            if hashlib.sha256(path.read_bytes()).hexdigest() == row.get('sha256') and unowned(name):
                path.unlink()
        except OSError:
            # Neither a locked file nor an unavailable package database blocks upgrades.
            continue


def remove_from_image(root):
    from package_bsp import owners

    ownership = None

    def unowned(name):
        nonlocal ownership
        if ownership is None:
            ownership = owners(root)
        return name not in ownership

    remove_legacy(root, unowned)


def remove_on_upgrade(package_format):
    if package_format == 'deb':
        command = ['/usr/bin/dpkg-query', '--search']
        missing = 'dpkg-query: no path found matching pattern '
    else:
        command = ['/usr/bin/pacman', '-Qoq']
        missing = 'error: No package owns '

    def unowned(name):
        path = '/' + name
        result = subprocess.run([*command, '--', path], capture_output=True, text=True,
                                env={'LC_ALL': 'C', 'PATH': '/usr/sbin:/usr/bin:/sbin:/bin'})
        return result.returncode == 1 and result.stderr.strip() == missing + path

    remove_legacy(Path('/'), unowned)
