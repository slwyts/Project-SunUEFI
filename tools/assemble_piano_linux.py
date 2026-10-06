#!/usr/bin/env python3
"""Verify/assemble one disabled Stable+Next EFI Linux file tree, host-only.

No firmware profile, block write, root repack or readiness override. Each kernel
uses the same immutable GNU-root bootstrap. Live DDR and late-EBS admission
remain runtime requirements even when all host artifact checks pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import struct

import build_piano_full_kernel as full
import build_piano_next_full as next_full
from build_kernel import image_info
from build_piano_full_dtb import libcheck, TRANSIENT, DEBIAN_COMMIT
from build_piano_ram_bootstrap import BUSYBOX_SHA
from package_piano_ram_root import sha_file, verify_archive, LIMIT, DOWNLOAD_TARGET
from prepare_linux_modules import modinfo

ROOT = Path(__file__).resolve().parents[1]
COMMITS = {'stable': 'd42158782b81c4aaa47c8643f1400a471785370b', 'next': next_full.COMMIT}
DEFAULT_KERNELS = {'stable': ROOT / 'artifacts/kernels/full-smmu-context',
                   'next': ROOT / 'artifacts/kernels/next-full' / next_full.COMMIT[:12]}
BLOCKERS = ('LIVE_FULL_DDR_EFI_MEMORY_CONTRACT_REQUIRED',
            'NATIVE_LATE_APP_EXIT_BOOT_SERVICES_OWNER_RETIREMENT_REQUIRED',
            'MATCHING_RUNTIME_HARDWARE_READBACK_REQUIRED')


def load(path):
    raw = Path(path).read_bytes()
    return json.loads(raw), hashlib.sha256(raw).hexdigest()


def safe_name(name):
    path = Path(name)
    if not name or path.is_absolute() or '..' in path.parts or '\x00' in name:
        raise ValueError('Unsafe artifact member path: ' + name)
    return name


def root_path(root, name):
    safe_name(name)
    path = root / name
    for part in (path, *path.parents):
        if part == root:
            break
        if part.is_symlink():
            raise ValueError('Root artifact path crosses symlink: ' + name)
    return path


def kernel(folder, family):
    manifest, pin = load(folder / 'manifest.json')
    if (manifest.get('status') != 'HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED' or
            manifest.get('source_commit') != COMMITS[family]):
        raise ValueError('Expected exact sealed full ' + family + ' kernel')
    info = image_info(folder / 'Image')
    if info != manifest['image'] or not info['efi_stub']:
        raise ValueError('Kernel is not the matching ARM64 EFI Image')
    config_pin = manifest.get('config_sha256')
    if config_pin is None:
        pins = [value for name, value in manifest.get('inputs', {}).items() if name.endswith('/.config')]
        if len(pins) != 1:
            raise ValueError('Exact kernel config pin missing')
        config_pin = pins[0]
    if sha_file(folder / 'config') != config_pin:
        raise ValueError('Kernel config changed')
    public_path = next_full.WORK / 'arch/arm64/configs/piano_rootfs.config'
    if sha_file(public_path) != full.SOURCE_PINS['arch/arm64/configs/piano_rootfs.config']:
        raise ValueError('Pinned public full profile changed')
    public = public_path.read_text()
    command = full.command_line((ROOT / 'configs/linux/piano-full.config').read_text(), public, 'ram')
    requirements = full.validate_config((folder / 'config').read_text(), public, command)
    if requirements != manifest['full_profile_requirements']:
        raise ValueError('Complete kernel requirements changed')
    release = manifest['kernel_release']
    if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,128}', release):
        raise ValueError('Invalid kernel release')
    rows, summary, required = next_full.validate_modules(folder / 'modules', manifest['kernel_release'])
    expected = dict(manifest['module_summary'])
    for key in ('all_modinfo_dependencies_resolved', 'complete_modules_dep_verified'):
        expected.setdefault(key, True)
    if rows != manifest['modules'] or summary != expected:
        raise ValueError('Sealed kernel modules/index changed')
    return {'family': family, 'source_commit': manifest['source_commit'],
            'release': manifest['kernel_release'], 'manifest_sha256': pin,
            'image': info, 'config_sha256': config_pin, 'command_line': command,
            'config_cmdline_force': True, 'external_debug_parameters_effective': False,
            'debug_parameter_limitation': 'CONFIG_CMDLINE_FORCE=y discards external debug bootargs; RAM distro /etc/piano/linux-debug.conf supplies validated USB/shell/network defaults',
            'public_bt_le_enabled': full.config_values((folder / 'config').read_text()).get('CONFIG_BT_LE') == 'y',
            'modules': rows, 'module_summary': summary, 'required_hardware_modules': required}


def tree_records(tree):
    records = tree['records']
    if not isinstance(records, list) or len(records) != tree['entries']:
        raise ValueError('Root tree count mismatch')
    names = {}
    for row in records:
        name = safe_name(row['name'])
        if name in names:
            raise ValueError('Duplicate root tree member')
        names[name] = row
    digest = hashlib.sha256(json.dumps(records, separators=(',', ':'), sort_keys=True).encode()).hexdigest()
    if digest != tree['tree_sha256']:
        raise ValueError('Root tree fingerprint mismatch')
    return names


def member_hash(rows, name):
    seen = set()
    while True:
        row = rows.get(name)
        if row is None or name in seen or not stat.S_ISREG(row['mode']):
            raise ValueError('Required regular root member missing: ' + name)
        seen.add(name)
        if 'hardlink' not in row:
            return row['sha256'], row['size']
        name = safe_name(row['hardlink'])


def root_modules(root, rows, kernels):
    result = {}
    for family, entry in kernels.items():
        release = entry['release']
        record_path = root_path(root, 'usr/share/piano-provenance/kernel-modules/' + release + '.json')
        record, record_sha = load(record_path)
        if (record.get('kernel_commit') != entry['source_commit'] or
                record.get('kernel_release') != release or
                record.get('kernel_manifest_sha256') != entry['manifest_sha256'] or
                record.get('kernel_image_sha256') != entry['image']['sha256']):
            raise ValueError('Root staged kernel provenance mismatch: ' + release)
        if member_hash(rows, str(record_path.relative_to(root)))[0] != record_sha:
            raise ValueError('Archive lacks matching staged kernel provenance: ' + release)
        originals = {}
        for module in entry['modules']:
            relative = Path(module['path']).relative_to('lib/modules/' + release)
            name = 'usr/lib/modules/' + release + '/' + relative.as_posix()
            path = root_path(root, name)
            if sha_file(path) != module['sha256'] or member_hash(rows, name) != (module['sha256'], module['bytes']):
                raise ValueError('Root/archive kernel module mismatch: ' + name)
            originals[name] = module['sha256']
        external = []
        root_library = root_path(root, 'usr/lib/modules/' + release)
        for path in sorted(root_library.rglob('*.ko')):
            name = path.relative_to(root).as_posix()
            if name in originals:
                continue
            info = modinfo(path)
            if info.get('name') != 'v4l2loopback' or path.parent.name != 'updates':
                raise ValueError('Unexpected external module: ' + name)
            if info.get('vermagic', '').split()[:1] != [release]:
                raise ValueError('External module ABI mismatch')
            original_names = {row['name'].replace('-', '_') for row in entry['modules']}
            if any(name.replace('-', '_') not in original_names for name in filter(None, info.get('depends', '').split(','))):
                raise ValueError('External module dependency is not in exact kernel set')
            digest = sha_file(path)
            if member_hash(rows, name) != (digest, path.stat().st_size):
                raise ValueError('Archive external module mismatch')
            external.append({'path': name, 'sha256': digest, 'bytes': path.stat().st_size,
                             'name': info['name'], 'vermagic': info['vermagic']})
        if len(external) != 1:
            raise ValueError('Each complete release requires its matching v4l2loopback module')
        dep = root_path(root, 'usr/lib/modules/' + release + '/modules.dep')
        if member_hash(rows, dep.relative_to(root).as_posix())[0] != sha_file(dep):
            raise ValueError('Archive root dependency index mismatch')
        expected_paths = {p.relative_to(root_library).as_posix() for p in root_library.rglob('*.ko')}
        indexed = set()
        for line in dep.read_text().splitlines():
            if ':' not in line:
                raise ValueError('Malformed staged modules.dep')
            name, dependencies = line.split(':', 1)
            if name not in expected_paths or name in indexed or any(x not in expected_paths for x in dependencies.split()):
                raise ValueError('Staged modules.dep incomplete or foreign dependency')
            indexed.add(name)
        if indexed != expected_paths:
            raise ValueError('Staged modules.dep omits real modules')
        result[family] = {'release': release, 'original_module_count': len(originals),
                          'external_modules': external, 'provenance_sha256': record_sha,
                          'root_modules_dep_sha256': sha_file(dep), 'all_archive_module_bytes_match': True}
    return result


def payload(folder, root, kernels):
    record, pin = load(folder / 'manifest.json')
    tree, tree_pin = load(folder / 'tree-manifest.json')
    if (record.get('status') != 'HOST_PACKAGED_GNU_ROOT_NOT_BOOT_VERIFIED' or
            record.get('root_policy') != 'RAM_ONLY_NO_ANDROID_BLOCK_MOUNT' or
            record.get('root_limit_bytes') != LIMIT):
        raise ValueError('Expected sealed complete GNU RAM root')
    if any(record.get(key) != tree.get(key) for key in ('tree_sha256', 'entries', 'regular_page_budget_bytes', 'regular_logical_bytes')):
        raise ValueError('Root manifest/tree identity mismatch')
    rows = tree_records(tree)
    # Reject an obsolete archive cheaply, before streaming a large payload.
    modules = root_modules(root, rows, kernels)
    for required_member in ('usr/lib/systemd/systemd', 'pianoinit', 'usr/lib/piano/piano-ram-hardware-prepare'):
        digest, size = member_hash(rows, required_member)
        source = root_path(root, required_member)
        if sha_file(source) != digest or source.stat().st_size != size:
            raise ValueError('Root staged executable differs from archive: ' + required_member)
    archive = folder / 'Piano-rootfs.tar.gz'
    if archive.stat().st_size != record['archive_bytes'] or sha_file(archive) != record['archive_sha256']:
        raise ValueError('Root payload bytes/hash changed')
    verification = verify_archive(archive, tree)
    if verification != record['archive_verification']:
        raise ValueError('Root archive member verification differs')
    if not 0 < record['regular_page_budget_bytes'] < LIMIT:
        raise ValueError('Expanded root exceeds RAM tmpfs budget')
    return {'manifest_sha256': pin, 'tree_manifest_sha256': tree_pin,
            'archive_sha256': record['archive_sha256'], 'archive_bytes': record['archive_bytes'],
            'expanded_root_page_budget_bytes': record['regular_page_budget_bytes'],
            'archive_verification': verification, 'kernel_modules': modules}, record


def bootstrap(folder, root_record):
    record, pin = load(folder / 'manifest.json')
    if (record.get('status') != 'HOST_BUILT_GNU_ROOT_BOOTSTRAP_NOT_BOOT_VERIFIED' or
            record.get('root_payload_sha256') != root_record['archive_sha256'] or
            record.get('root_limit_bytes') != LIMIT or record.get('root_extraction_passes') != 1 or
            record.get('expanded_root_page_budget_bytes') != root_record['regular_page_budget_bytes']):
        raise ValueError('Expected matching GNU root bootstrap')
    path = folder / 'initramfs.cpio'
    if path.stat().st_size != record['initramfs_bytes'] or sha_file(path) != record['initramfs_sha256']:
        raise ValueError('Bootstrap hash/size changed')
    wanted = dict(record['gnu_tar_runtime'])
    wanted.update({'bin/busybox': {'sha256': BUSYBOX_SHA},
                   'pianoinit': {'sha256': record['entry_sha256']},
                   'rootfs.tar.gz': {'sha256': root_record['archive_sha256'], 'bytes': root_record['archive_bytes']}})
    seen = set()
    small = {}
    with path.open('rb') as stream:
        total = path.stat().st_size
        while True:
            header = stream.read(110)
            if len(header) != 110 or header[:6] != b'070701':
                raise ValueError('Invalid/truncated bootstrap newc header')
            try:
                fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
            except ValueError as error:
                raise ValueError('Invalid newc fields') from error
            mode, size, namesize = fields[1], fields[6], fields[11]
            if not 1 <= namesize <= 4096 or stream.tell() + namesize > total:
                raise ValueError('Invalid bootstrap member name bound')
            raw = stream.read(namesize)
            if raw[-1:] != b'\0':
                raise ValueError('Unterminated bootstrap member name')
            name = safe_name(raw[:-1].decode())
            if name in seen:
                raise ValueError('Duplicate bootstrap member')
            seen.add(name)
            pad = -stream.tell() % 4
            if stream.read(pad) != bytes(pad) or stream.tell() + size > total:
                raise ValueError('Invalid bootstrap data alignment/bound')
            digest = hashlib.sha256(); data = bytearray()
            remaining = size
            while remaining:
                chunk = stream.read(min(remaining, 1024**2))
                if not chunk:
                    raise ValueError('Short bootstrap member')
                digest.update(chunk); remaining -= len(chunk)
                if name in ('rootfs.sha256', 'rootfs.page-budget') and size <= 4096:
                    data.extend(chunk)
            if name in wanted:
                expected = wanted[name]
                if not stat.S_ISREG(mode) or digest.hexdigest() != expected['sha256'] or ('bytes' in expected and size != expected['bytes']):
                    raise ValueError('Bootstrap runtime/payload member differs: ' + name)
            if data:
                small[name] = bytes(data)
            pad = -stream.tell() % 4
            if stream.read(pad) != bytes(pad):
                raise ValueError('Bootstrap member padding differs')
            if name == 'TRAILER!!!':
                if size or any(stream.read()):
                    raise ValueError('Invalid bootstrap trailer')
                break
    if record.get('busybox_sha256', BUSYBOX_SHA) != BUSYBOX_SHA:
        raise ValueError('Bootstrap BusyBox identity differs')
    if not set(wanted).issubset(seen):
        raise ValueError('Bootstrap omits GNU runtime/payload')
    if small.get('rootfs.sha256') != (root_record['archive_sha256'] + '  rootfs.tar.gz\n').encode() or small.get('rootfs.page-budget') != (str(root_record['regular_page_budget_bytes']) + '\n').encode():
        raise ValueError('Bootstrap root hash/page-budget metadata differs')
    return {'manifest_sha256': pin, 'bytes': record['initramfs_bytes'],
            'sha256': record['initramfs_sha256'], 'embedded_root_payload_verified': True}


def device_tree(path, manifests, library):
    if not manifests:
        raise ValueError('Full DTB fold provenance is required')
    first, _ = load(manifests[0])
    if (first.get('source_commits', {}).get('debian') != DEBIAN_COMMIT or
            first.get('selected_rom') != {'dtb_index': 4, 'dtbo_index': 0} or
            first.get('memory_ownership_authorized') is not False):
        raise ValueError('Expected current-ROM complete fold provenance')
    files_by_status = {
        'HOST_FOLDED_KERNEL_ROUTE_READBACK_REQUIRED': 'Piano-full-linux-owned-dma.dtb',
        'HOST_CLOCK_BINDINGS_FOLDED_NORMAL_DRIVER_READBACK_REQUIRED': 'Piano-full-linux-managed-clocks.dtb',
        'HOST_DSP_PCIE_BINDINGS_FOLDED_KERNEL_READBACK_REQUIRED': 'Piano-full-linux-managed-dsp-pcie.dtb',
        'HOST_KEYBOARD_SUPPLIERS_FOLDED_KERNEL_READBACK_REQUIRED': 'Piano-full-linux-keyboard-suppliers.dtb',
    }
    previous = None
    before = None
    pins = []
    for index, manifest in enumerate(manifests):
        record, pin = load(manifest)
        pins.append({'path': str(manifest), 'sha256': pin})
        if index == 0:
            candidate = manifest.parent / safe_name(record['output']['file'])
            expected_sha, expected_bytes = record['output']['sha256'], record['output']['bytes']
        else:
            name = files_by_status.get(record.get('status'))
            if not name or record.get('base_sha256') != previous:
                raise ValueError('Unknown/broken DTB provenance stage')
            if record.get('memory_ownership_authorized', False) is not False:
                raise ValueError('Host DTB stage must not authorize memory')
            candidate = manifest.parent / name
            expected_sha, expected_bytes = record['output_sha256'], record['output_bytes']
        blob = candidate.read_bytes()
        if hashlib.sha256(blob).hexdigest() != expected_sha or len(blob) != expected_bytes:
            raise ValueError('DTB stage output bytes differ')
        parsed = libcheck(blob, library)
        if before is not None:
            usb_supply_fixes = set()
            added = set(parsed['tree']) - set(before['tree'])
            if record['status'] == 'HOST_DSP_PCIE_BINDINGS_FOLDED_KERNEL_READBACK_REQUIRED' and added:
                from apply_piano_dsp_pcie_masters import (USB_RAIL_NODES, USB_SUPPLY_FIX_FIELDS,
                                                        validate_usb_supply_delta)
                if added != USB_RAIL_NODES or record.get('new_nodes') != sorted(USB_RAIL_NODES):
                    raise ValueError('DTB stage added unaudited regulator nodes')
                validate_usb_supply_delta(before, parsed)
                usb_supply_fixes = USB_SUPPLY_FIX_FIELDS
            if (set(before['tree']) != set(parsed['tree']) or
                    before['reservations'] != parsed['reservations'] or
                    before['phandles'] != parsed['phandles']) and not usb_supply_fixes:
                raise ValueError('DTB stage changed node/reservation/phandle identity')
            actual = {(node, prop) for node in before['tree']
                      for prop in set(before['tree'][node]) | set(parsed['tree'][node])
                      if before['tree'][node].get(prop) != parsed['tree'][node].get(prop)}
            declared = {(row['path'], row['property']) for row in record['changes']}
            if actual != declared:
                raise ValueError('DTB stage actual property changes differ from manifest')
            if usb_supply_fixes and actual & usb_supply_fixes != usb_supply_fixes:
                raise ValueError('Incomplete USB supply reference repair')
            allowed_properties = {'iommus'} if record['status'] == 'HOST_FOLDED_KERNEL_ROUTE_READBACK_REQUIRED' else (
                {'clocks', 'compatible'} if record['status'] == 'HOST_CLOCK_BINDINGS_FOLDED_NORMAL_DRIVER_READBACK_REQUIRED' else {'iommus', 'iommu-map'})
            boot_fixes = set()
            usb_fixes = set()
            keyboard_fixes = set()
            if record['status'] == 'HOST_KEYBOARD_SUPPLIERS_FOLDED_KERNEL_READBACK_REQUIRED':
                from apply_piano_keyboard_suppliers import FIX_FIELDS, validate_delta
                if actual != FIX_FIELDS or record.get('new_nodes') != []:
                    raise ValueError('Incomplete or unaudited keyboard supplier repair')
                validate_delta(before, parsed)
                keyboard_fixes = FIX_FIELDS
            if record['status'] == 'HOST_DSP_PCIE_BINDINGS_FOLDED_KERNEL_READBACK_REQUIRED':
                from apply_piano_dsp_pcie_masters import (BOOT_FIX_FIELDS, USB_FIX_FIELDS,
                                                        validate_boot_fix_delta, validate_usb_fix_delta)
                if actual & BOOT_FIX_FIELDS:
                    if actual & BOOT_FIX_FIELDS != BOOT_FIX_FIELDS:
                        raise ValueError('Incomplete boot geometry repair')
                    validate_boot_fix_delta(before, parsed)
                    boot_fixes = BOOT_FIX_FIELDS
                if actual & USB_FIX_FIELDS:
                    if actual & USB_FIX_FIELDS != USB_FIX_FIELDS:
                        raise ValueError('Incomplete USB PHY chain repair')
                    validate_usb_fix_delta(before, parsed)
                    usb_fixes = USB_FIX_FIELDS
            if any(prop not in allowed_properties or not node.startswith('/soc/')
                   for node, prop in actual - boot_fixes - usb_fixes - usb_supply_fixes - keyboard_fixes):
                raise ValueError('DTB stage changed an unaudited property class')
        previous, before = expected_sha, parsed
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != previous:
        raise ValueError('Final DTB hash differs from complete fold chain')
    if any(key in before['tree'].get('/chosen', {}) for key in TRANSIENT):
        raise ValueError('Old initrd/random seed survived DTB fold')
    return {'bytes': len(data), 'sha256': previous, 'fold_manifests': pins,
            'nodes': len(before['tree']), 'memory_ownership_authorized': False,
            'retained_risks': first.get('retained_risks', []),
            'diagnostic_impact': first.get('diagnostic_impact', {}), 'requires_live_memory_admission': True}


def assemble(stable, next_kernel, rootfs, root_payload, ram_bootstrap, dtb,
             dtb_manifests, libfdt, output=None):
    kernels = {name: kernel(Path(folder), name) for name, folder in [('stable', stable), ('next', next_kernel)]}
    dt = device_tree(Path(dtb), [Path(p) for p in dtb_manifests], Path(libfdt))
    root, root_record = payload(Path(root_payload), Path(rootfs), kernels)
    initrd = bootstrap(Path(ram_bootstrap), root_record)
    entries = {}
    files = {}
    for family, folder in [('stable', Path(stable)), ('next', Path(next_kernel))]:
        entry = kernels[family]
        base = '/EFI/Piano/' + family
        total = entry['image']['bytes'] + dt['bytes'] + initrd['bytes']
        if total > DOWNLOAD_TARGET:
            raise ValueError('Kernel/DTB/shared bootstrap exceeds 1GiB source budget')
        entries[family] = {'enabled': False, 'admission': 'DISABLED_REQUIRES_LIVE_DDR_AND_LATE_EBS',
                           'blockers': list(BLOCKERS), 'efi_image': base + '/Image.efi',
                           'device_tree': base + '/piano.dtb',
                           'initramfs': '/EFI/Piano/shared/initramfs.cpio',
                           'load_options': entry['command_line'], 'source_bytes': total,
                           'kernel_release': entry['release'], 'kernel_source_commit': entry['source_commit'],
                           'external_debug_parameters_effective': False}
        for name, source in [('Image.efi', folder / 'Image'), ('config', folder / 'config'), ('piano.dtb', Path(dtb))]:
            target = base + '/' + name
            files[target] = {'source': str(source.resolve()), 'bytes': source.stat().st_size, 'sha256': sha_file(source)}
    files['/EFI/Piano/shared/initramfs.cpio'] = {'source': str((Path(ram_bootstrap) / 'initramfs.cpio').resolve()), **{key: initrd[key] for key in ('bytes', 'sha256')}}
    report = {'schema': 1, 'status': 'HOST_VERIFIED_LINUX_INPUT_SET_DISABLED',
              'entries': entries, 'files': files, 'device_tree': dt, 'gnu_root': root,
              'shared_bootstrap': initrd, 'memory_admission': 'LIVE_RUNTIME_UNPROVED',
              'late_ebs_admission': 'LIVE_RUNTIME_UNPROVED', 'hardware_verified': False,
              'device_operation_performed': False, 'no_firmware_profile_created': True,
              'tool_sha256': sha_file(Path(__file__))}
    if output is not None:
        materialize(files, report, output)
    return report


def materialize(files, report, output):
    """Copy a verified input set to a fresh host artifact; keep entries disabled."""
    output = Path(output).resolve()
    if not output.is_relative_to(ROOT / 'artifacts/linux-assembled') or output.exists():
        raise ValueError('EFI tree output must be a fresh artifacts/linux-assembled directory')
    if any(entry.get('enabled') is not False for entry in report['entries'].values()):
        raise ValueError('Host assembler cannot enable a runtime boot entry')
    output.mkdir(parents=True)
    for name, row in files.items():
        relative = safe_name(name.lstrip('/'))
        if not name.startswith('/EFI/Piano/') or sha_file(Path(row['source'])) != row['sha256']:
            raise ValueError('Invalid/drifted EFI input file')
        dest = output / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(row['source'], dest)
        if sha_file(dest) != row['sha256']:
            raise ValueError('Assembled file copy changed')
    for name, row in files.items():
        if sha_file(Path(row['source'])) != row['sha256']:
            raise ValueError('Assembly source changed during copying: ' + name)
    report['status'] = 'HOST_ASSEMBLED_LINUX_EFI_TREE_DISABLED'
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stable', type=Path, default=DEFAULT_KERNELS['stable'])
    parser.add_argument('--next', dest='next_kernel', type=Path, default=DEFAULT_KERNELS['next'])
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--root-payload', type=Path, required=True)
    parser.add_argument('--bootstrap', type=Path, required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--dtb-manifest', type=Path, action='append', required=True)
    parser.add_argument('--libfdt', type=Path, default=ROOT / 'upstream/dtc/libfdt/libfdt.so.1.8.1')
    parser.add_argument('--output', type=Path, help='Explicitly materialize a new EFI tree; default verifies without copying')
    args = parser.parse_args()
    print(json.dumps(assemble(args.stable, args.next_kernel, args.rootfs, args.root_payload,
                             args.bootstrap, args.dtb, args.dtb_manifest, args.libfdt, args.output), indent=2))


if __name__ == '__main__':
    main()
