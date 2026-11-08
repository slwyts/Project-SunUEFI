#!/usr/bin/env python3
"""Build the native ARM64 Steam bootstrap from pinned public CDN packages."""
import concurrent.futures
import calendar
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import zipfile

from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]


def source_record():
    return {**json.loads((ROOT / 'config/steam-client.json').read_text()),
            'builder_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def enable_first_launch(root):
    launcher = root / 'usr/lib/steam/bin_steam.sh'
    text = launcher.read_text()
    old = 'exec "$LAUNCHSTEAMDIR/$LAUNCHSTEAMPLATFORM/steam" "$@"'
    new = '''# SunUEFI: Frame assumes its client has already been installed by SteamVR.
# Use the launcher's own bootstrap functions for an ordinary first login.
if ! check_bootstrap "$LAUNCHSTEAMDIR" "$*"; then
    install_bootstrap "$DEFAULTSTEAMDIR"
fi
ln -fns "$LAUNCHSTEAMDIR" "$STEAMCONFIG/root"
ln -fns "$LAUNCHSTEAMDIR/linuxarm64" "$STEAMCONFIG/sdkarm64"
export LD_LIBRARY_PATH="$LAUNCHSTEAMDIR/$LAUNCHSTEAMPLATFORM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$LAUNCHSTEAMDIR"
mkdir -p logs
while :; do
    status=0
    "$LAUNCHSTEAMDIR/$LAUNCHSTEAMPLATFORM/steam" "$@" >>logs/console-linux.txt 2>&1 || status=$?
    [ "$status" = 42 ] || exit "$status"
done'''
    if new in text:
        return
    previous = new.replace(
        'ln -fns "$LAUNCHSTEAMDIR/linuxarm64" "$STEAMCONFIG/sdkarm64"\n', ''
    ).replace('mkdir -p logs\n', '').replace(' >>logs/console-linux.txt 2>&1', '')
    if previous in text:
        launcher.write_text(text.replace(previous, new))
        return
    if text.count(old) != 1:
        raise ValueError('Steam launcher first-run entry changed')
    launcher.write_text(text.replace(old, new))


def prepare(root, output):
    cfg = source_record()
    cache = output / 'downloads'
    tree = output / 'client'
    cache.mkdir(parents=True, exist_ok=True)
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir()
    def fetch(item):
        name, row = item
        path = cache / row['file']
        download('https://client-update.akamai.steamstatic.com/' + row['file'],
                 path, row['sha2'])
        if path.stat().st_size != int(row['size']):
            raise ValueError('Steam package size mismatch: ' + name)
        return path
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(fetch, cfg['packages'].items()))
    installed = {}
    for archive in archives:
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                # Valve's cross-platform ZIPs use backslashes even for Linux.
                name = Path(entry.filename.replace('\\', '/'))
                if name.is_absolute() or '..' in name.parts:
                    raise ValueError('Invalid Steam archive path')
                target = tree / name
                directory = entry.is_dir() or entry.filename.endswith('\\')
                manifest_name = name.as_posix() + ('/' if directory else '')
                timestamp = calendar.timegm((*entry.date_time, 0, 0, 0))
                mode = entry.external_attr >> 16
                installed[manifest_name] = (
                    -1 if directory else -2 if stat.S_ISLNK(mode) else entry.file_size,
                    timestamp, entry.CRC)
                if directory:
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                # Do not let an earlier archive's symlink redirect extraction.
                if target.is_symlink() or any(p.is_symlink() for p in target.parents if p != tree):
                    raise ValueError('Steam archive writes through a symlink')
                target.parent.mkdir(parents=True, exist_ok=True)
                if stat.S_ISLNK(mode):
                    link = source.read(entry).decode()
                    resolved = (target.parent / link).resolve()
                    if Path(link).is_absolute() or not resolved.is_relative_to(tree.resolve()):
                        raise ValueError('Steam archive link escapes its root')
                    target.symlink_to(link)
                    continue
                with source.open(entry) as incoming, target.open('wb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
                with target.open('rb') as stream:
                    elf = stream.read(4) == b'\x7fELF'
                target.chmod(0o755 if elf or name.suffix == '.sh' or mode & 0o111 else 0o644)
    for required in ('steam.sh', 'steamrtarm64/steam', 'steamrtarm64/steamui.so'):
        if not (tree / required).is_file():
            raise ValueError('Native client is incomplete: ' + required)
    (tree / 'package').mkdir(exist_ok=True)
    (tree / 'package/beta').write_text(cfg['channel'] + '\n')
    # Steam's inventory uses -1 for directories and -2 for symlinks. A link's
    # CRC covers its target text, not the referenced file's contents.
    inventory = 'steam_client_' + cfg['channel'] + '_' + cfg['platform'] + '.installed'
    inventory_text = ''.join(
        f'{name},{size};{timestamp};{crc}\n'
        for name, (size, timestamp, crc) in sorted(installed.items()))
    # Native linuxarm64 client inventory format, as emitted by this pinned
    # client. SHA1 covers every preceding byte including OSVER and VERSION.
    inventory_text += 'OSVER=-183\nVERSION=3\n'
    inventory_text += 'SHA1=' + hashlib.sha1(inventory_text.encode()).hexdigest().upper() + '\n'
    (tree / 'package' / inventory).write_text(inventory_text)
    destination = root / 'usr/lib/steam/bootstraplinux_steamrtarm64.tar.xz'
    subprocess.run(['tar', '--sort=name', '--mtime=@0', '--owner=0', '--group=0',
                    '-I', 'xz -T4 -1', '-cf', str(destination), '-C', str(tree), '.'], check=True)
    # The Frame tarball is superseded by this complete handheld bootstrap.
    (root / 'usr/lib/steam/steam.tar.zst').unlink(missing_ok=True)
    enable_first_launch(root)
    shutil.rmtree(tree)
    (output / 'SOURCE.json').write_text(json.dumps(cfg, indent=2) + '\n')
    return cfg
