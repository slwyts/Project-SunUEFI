#!/usr/bin/env python3
"""Stage the shared hardware runtime without ordering driver module loads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REMOVED_SERVICES = ('display', 'radio', 'audio', 'video', 'camera', 'keyboard', 'cpufreq')
REMOVED_SCRIPTS = ('display', 'radio', 'audio', 'video', 'camera', 'keyboard', 'touch', 'adsp', 'cpufreq')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_destination(rootfs, relative):
    path = rootfs / relative
    for parent in path.parents:
        if parent == rootfs:
            break
        if parent.is_symlink():
            raise ValueError('guest symlink in hardware runtime destination: ' + str(parent))
    return path


def removed_paths(rootfs):
    paths = {'etc/udev/rules.d/80-drivers.rules',
             'etc/systemd/system/display-manager.service.d/20-piano-display.conf'}
    for name in REMOVED_SCRIPTS:
        paths.add('usr/lib/piano/' + name + '-start')
    for name in REMOVED_SERVICES:
        unit = 'piano-' + name + '.service'
        paths.add('usr/lib/systemd/system/' + unit)
        paths.add('etc/systemd/system/' + unit)
        for base in ('etc/systemd/system', 'usr/lib/systemd/system'):
            for link in (rootfs / base).glob('*.wants/' + unit):
                paths.add(link.relative_to(rootfs).as_posix())
            for dropin in (rootfs / base / (unit + '.d')).glob('*.conf'):
                paths.add(dropin.relative_to(rootfs).as_posix())
    paths.add('etc/systemd/system/multi-user.target.wants/piano-touch.service')
    paths.add('etc/systemd/system/multi-user.target.wants/piano-usb.service')
    return sorted(paths)


def build(output, source, rootfs=None):
    output, source = Path(output).resolve(), Path(source).resolve()
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        raise ValueError('output must be a new workspace build directory')
    pin = json.loads((ROOT / 'sources.lock.json').read_text())['debian-piano-current']['commit']
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != pin or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True):
        raise ValueError('public source must have the pinned clean commit')
    files = {}
    common = ROOT / 'linux/bsp/common'
    for path in sorted(common.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            files[path.relative_to(common).as_posix()] = path.read_bytes()
    files['usr/lib/piano/piano-adsp-start'] = (ROOT / 'linux/userspace/piano-adsp-start').read_bytes()
    # These remain explicit diagnostics, never boot gates or module loaders.
    for relative, source_name in (
        ('usr/lib/piano/piano_dma_routes.py', 'tools/check_piano_kernel_dma_routes.py'),
        ('usr/lib/piano/piano_dma_contexts.py', 'tools/check_piano_kernel_contexts.py'),
        ('usr/lib/piano/piano-ram-hardware-prepare', 'linux/userspace/piano-ram-hardware-prepare'),
    ):
        files[relative] = (ROOT / source_name).read_bytes()
    removed = []
    if rootfs:
        rootfs = Path(rootfs).resolve()
        if not rootfs.is_relative_to(ROOT / 'build/distros') or not rootfs.is_dir():
            raise ValueError('stage target must be an existing derived build/distros tree')
        removed = removed_paths(rootfs)
        for name in (*files, *removed):
            safe_destination(rootfs, name)
    output.mkdir(parents=True)
    for name, data in files.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755 if name.startswith('usr/lib/piano/') and not name.endswith('.py') else 0o644)
    subprocess.run(['/bin/sh', '-n', str(output / 'usr/lib/piano/piano-adsp-start')], check=True)
    result = {'status': 'HARDWARE_RUNTIME_STAGED', 'source_commit': commit,
              'files': {name: digest(data) for name, data in files.items()},
              'removed_paths': removed, 'driver_loading': 'standard-modalias',
              'device_tested': False, 'global_ready_marker': False, 'register_programming': False}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    if rootfs:
        for name in removed:
            path = safe_destination(rootfs, name)
            if path.is_file() or path.is_symlink():
                path.unlink()
        for name in files:
            destination = safe_destination(rootfs, name)
            if destination.is_symlink():
                raise ValueError('Refuse writing through a guest symlink: ' + name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(output / name, destination)
        wants = rootfs / 'etc/systemd/system/multi-user.target.wants'
        wants.mkdir(parents=True, exist_ok=True)
        link = wants / 'piano-adsp.service'
        if not link.exists() and not link.is_symlink():
            link.symlink_to('/usr/lib/systemd/system/piano-adsp.service')
        manifest = rootfs / 'usr/share/piano-provenance/ram-hardware.json'
        manifest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / 'manifest.json', manifest)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'upstream/debian-piano-current')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--rootfs', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output_dir, args.source, args.rootfs), indent=2))


if __name__ == '__main__':
    main()
