#!/usr/bin/env python3
"""Package a small ARM64 initramfs for the explicitly allocated Linux partition.

Host packaging only: reuses the frozen kernel/modules, static BusyBox and distro
blkid. Does not partition, mount, format, build a kernel or access a device.
"""
import argparse
import gzip
import json
from pathlib import Path
import stat
import uuid

from build_piano_ram_bootstrap import BUSYBOX_SHA, elf_dependencies, guest_resolve
from make_kernel_initramfs import ROOT, inspect_newc, make_newc, validate_static_arm64_elf
from package_piano_ram_root import sha_file

APPLETS = ('sh', 'cat', 'mkdir', 'mount', 'mountpoint', 'chmod', 'uname', 'chroot',
           'switch_root', 'sleep', 'insmod', 'umount', 'grep')
SEEDS = ('arm_smmu', 'pinctrl_sm8750', 'phy_qcom_qmp_ufs', 'ufs_qcom')


def runtime_files(root):
    pending, files = ['/usr/sbin/blkid'], {}
    while pending:
        name = pending.pop(0)
        if name.lstrip('/') in files:
            continue
        source = guest_resolve(root, name)
        if not source.is_file():
            raise ValueError('Missing blkid runtime: ' + name)
        needed, interpreters = elf_dependencies(source)
        files[name.lstrip('/')] = source
        pending += interpreters
        for soname in needed:
            if '/' in soname:
                raise ValueError('Unexpected ELF dependency path')
            pending.append('/lib/aarch64-linux-gnu/' + soname)
    return files


def module_name(path):
    return Path(path).name.removesuffix('.ko').replace('-', '_')


def module_closure(directory):
    deps = dict(line.split(':', 1) for line in (directory / 'modules.dep').read_text().splitlines())
    names = {module_name(path): path for path in deps}
    builtin = {module_name(path) for path in (directory / 'modules.builtin').read_text().splitlines()}
    softdeps = {}
    for line in (directory / 'modules.softdep').read_text().splitlines():
        words = line.split()
        if words and words[0] == 'softdep':
            # No current selected module has post dependencies; preserve both.
            softdeps[words[1].replace('-', '_')] = [word.replace('-', '_') for word in words[2:]
                                                     if word not in ('pre:', 'post:')]
    ordered, seen, active, used_builtin = [], set(), set(), set()

    def visit(name):
        if name in builtin:
            used_builtin.add(name)
            return
        if name in seen:
            return
        if name in active or name not in names:
            raise ValueError('Missing or cyclic module dependency: ' + name)
        active.add(name)
        path = names[name]
        for dependency in softdeps.get(name, []):
            visit(dependency)
        for dependency in deps[path].split():
            visit(module_name(dependency))
        active.remove(name)
        seen.add(name)
        ordered.append(path)

    for seed in SEEDS:
        visit(seed)
    return ordered, sorted(used_builtin)


def build(root, kernel, busybox, output, root_partuuid):
    root, kernel, busybox, output = [Path(path).resolve() for path in (root, kernel, busybox, output)]
    root_partuuid = str(uuid.UUID(root_partuuid))
    if not root.is_relative_to(ROOT / 'build/distros') or not kernel.is_relative_to(ROOT / 'artifacts/kernels'):
        raise ValueError('Use the existing workspace distro and kernel artifacts')
    if not output.is_relative_to(ROOT / 'artifacts') or output.exists():
        raise ValueError('Output must be a fresh workspace artifact directory')
    record = json.loads((kernel / 'manifest.json').read_text())
    release = record['kernel_release']
    if '/' in release or not release:
        raise ValueError('Invalid kernel release')
    if sha_file(kernel / 'Image') != record['image']['sha256'] or sha_file(kernel / 'config') != record['config_sha256']:
        raise ValueError('Kernel artifacts differ from their manifest')
    if sha_file(busybox) != BUSYBOX_SHA:
        raise ValueError('BusyBox differs from its known source artifact')
    validate_static_arm64_elf(busybox.read_bytes())
    files = runtime_files(root)
    directory = kernel / 'modules/lib/modules' / release
    ordered, builtin = module_closure(directory)
    module_records = {row['path']: row for row in record['modules']}
    for path in ordered:
        name = f'lib/modules/{release}/{path}'
        source = directory / path
        expected = module_records[name]
        if sha_file(source) != expected['sha256'] or not expected['vermagic'].startswith(release + ' '):
            raise ValueError('Unverified module: ' + path)
        if sha_file(guest_resolve(root, '/' + name)) != expected['sha256']:
            raise ValueError('Distro module differs from selected kernel: ' + path)
        files[name] = source
    files['bin/busybox'] = busybox
    files['pianoinit'] = ROOT / 'bootprofiles/linux-userspace/disk-bootstrap'
    files['etc/piano/busybox-source.json'] = busybox.with_name('busybox-source.json')
    files[f'lib/modules/{release}/modules.builtin'] = directory / 'modules.builtin'
    generated = {
        'etc/piano/root-partuuid': root_partuuid + '\n',
        'etc/piano/kernel-release': release + '\n',
        'etc/piano/modules-load-order': ''.join(f'{module_name(path)} /lib/modules/{release}/{path}\n' for path in ordered),
        f'lib/modules/{release}/modules.dep': ''.join(line + '\n' for line in (directory / 'modules.dep').read_text().splitlines()
                                                     if line.split(':', 1)[0] in ordered),
    }
    directories = {'dev', 'proc', 'sys', 'run', 'tmp', 'sysroot'}
    for name in files.keys() | generated.keys():
        directories.update(parent.as_posix() for parent in Path(name).parents if parent.as_posix() != '.')
    entries = [{'name': name, 'mode': stat.S_IFDIR | (0o1777 if name == 'tmp' else 0o755)}
               for name in sorted(directories, key=lambda name: (name.count('/'), name))]
    entries += [{'name': 'dev/console', 'mode': stat.S_IFCHR | 0o600, 'major': 5, 'minor': 1},
                {'name': 'dev/null', 'mode': stat.S_IFCHR | 0o666, 'major': 1, 'minor': 3}]
    rows = {name: {'bytes': path.stat().st_size, 'sha256': sha_file(path)} for name, path in files.items()}
    entries += [{'name': name, 'mode': stat.S_IFREG | 0o755, 'data': path.read_bytes()} for name, path in sorted(files.items())]
    entries += [{'name': name, 'mode': stat.S_IFREG | 0o644, 'data': data.encode()} for name, data in sorted(generated.items())]
    entries += [{'name': 'bin/' + name, 'mode': stat.S_IFLNK | 0o777, 'data': b'busybox'} for name in APPLETS]
    archive = make_newc(entries)
    if [row['name'] for row in inspect_newc(archive)] != [row['name'] for row in entries]:
        raise ValueError('Archive round-trip changed its contents')
    if any(sha_file(files[name]) != row['sha256'] for name, row in rows.items()):
        raise ValueError('Source changed while packing')
    output.mkdir(parents=True)
    target = output / 'initramfs.cpio.gz'
    target.write_bytes(gzip.compress(archive, mtime=0))
    result = {'status': 'HOST_BUILT_DISK_BOOTSTRAP_NOT_BOOT_VERIFIED', 'kernel_release': release,
              'root_partuuid': root_partuuid, 'root_partlabel': 'sunuefi_linux', 'root_fstype': 'ext4',
              'kernel_root_policy': record.get('root_policy'),
              'kernel_cmdline_matches_root': f'piano.root=PARTUUID={root_partuuid}' in record['command_line'].split(),
              'initramfs_bytes': target.stat().st_size, 'initramfs_sha256': sha_file(target),
              'module_load_order': ordered, 'builtin_dependencies': builtin, 'files': rows,
              'kernel_manifest_sha256': sha_file(kernel / 'manifest.json'), 'tool_sha256': sha_file(__file__),
              'device_operation_performed': False, 'piano_boot_verified': False}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'artifacts/kernels/full-smmu-context')
    parser.add_argument('--busybox', type=Path, default=ROOT / 'build/linux-ram/busybox')
    parser.add_argument('--root-partuuid', default='ffc480ed-c219-400b-a8f9-5f6805aa1f34')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = build(args.rootfs, args.kernel, args.busybox, args.output, args.root_partuuid)
    print(json.dumps({key: value for key, value in result.items() if key != 'files'}, indent=2))
