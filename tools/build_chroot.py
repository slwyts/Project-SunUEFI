"""Keep build commands and their helpers inside temporary chroot environments."""
import json
import os
from pathlib import Path
import re
import select
import signal
import stat
import sys
import time
from urllib.parse import urlsplit


def guest_command(root, args):
    """Return after the command's descendants have exited, even after chroot changes."""
    root = Path(root)
    return ['unshare', '--pid', '--fork', '--kill-child',
            '--mount-proc=' + str(root / 'proc'), 'chroot', str(root),
            *map(str, args)]


def arch_mirror_url(url):
    address = urlsplit(url)
    if (address.scheme != 'https' or not address.hostname or address.username is not None or
            '?' in url or '#' in url or any(char.isspace() for char in url)):
        raise ValueError('The Arch Linux ARM build mirror must be a public HTTPS URL')
    return url.rstrip('/')


def pacman_build_config(root, *, mirror=None):
    """Select an Arch build mirror without editing the installed configuration."""
    root = Path(root)
    if mirror is None and root.name == 'rootfs':
        manifest = root.parent / 'manifest.json'
        if manifest.is_file():
            record = json.loads(manifest.read_text())
            if record.get('distro') == 'arch':
                mirror = record.get('pacman_build_mirror')

    text = (root / 'etc/pacman.conf').read_text()
    # The host mount paths are not reachable from the build chroot.
    text = re.sub(r'^\s*CheckSpace\s*$', '# CheckSpace (build chroot)', text, flags=re.M)
    if mirror is None:
        return text
    server = arch_mirror_url(mirror) + '/$arch/$repo'
    text, includes = re.subn(r'^([ \t]*)Include[ \t]*=[ \t]*/etc/pacman\.d/mirrorlist[ \t]*(?:#.*)?$',
                            lambda match: match[1] + 'Server = ' + server, text, flags=re.M)
    if not includes:
        raise ValueError('Arch Linux ARM configuration does not include /etc/pacman.d/mirrorlist')
    return text


def remove_pacman_sockets(root):
    """Remove dead IPC endpoints, not pacman's keys, trust or package state."""
    directory = Path(root) / 'etc/pacman.d/gnupg'
    for path in directory.glob('S.*'):
        if stat.S_ISSOCK(path.lstat().st_mode):
            path.unlink()


def stop_helpers(root):
    """Terminate only processes whose chroot is this build's root directory."""
    root = Path(root).resolve()
    if root == Path('/'):
        raise ValueError('Refusing to stop processes in the host root')
    pending = {}
    poller = select.poll()
    try:
        for proc in Path('/proc').glob('[0-9]*'):
            fd = None
            try:
                if not (proc / 'root').samefile(root):
                    continue
                fd = os.pidfd_open(int(proc.name))
                # The PID may have exited or been reused while opening it.
                if not (proc / 'root').samefile(root):
                    os.close(fd)
                    continue
                print(f'Stopping build helper {proc.name} in {root}', file=sys.stderr)
                signal.pidfd_send_signal(fd, signal.SIGTERM)
                poller.register(fd, select.POLLIN)
                pending[fd] = proc.name
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                if fd is not None:
                    os.close(fd)
        # GnuPG's --kill command requests shutdown but need not wait for exit.
        # pidfds let us wait without confusing zombies or reused PIDs.
        for timeout, terminate in ((2000, False), (1000, True)):
            if not pending:
                break
            if terminate:
                for fd in pending:
                    try:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            deadline = time.monotonic() + timeout / 1000
            while pending:
                remaining = max(0, int((deadline - time.monotonic()) * 1000))
                for fd, _ in poller.poll(remaining):
                    poller.unregister(fd)
                    os.close(fd)
                    del pending[fd]
                if time.monotonic() >= deadline:
                    break
        if pending:
            raise RuntimeError(f'Build helpers did not exit: {list(pending.values())}')
    finally:
        for fd in pending:
            os.close(fd)
