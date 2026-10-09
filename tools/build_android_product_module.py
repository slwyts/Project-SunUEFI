#!/usr/bin/env python3
"""Build the same-product Android module ZIP without a stock BOOT or device.

The module extracts the current device BOOT during installation. Native tools
and filesystem tools are static ARM64 executables; both x86 and ARM builders
use an ARM64 sysroot. This build never changes native/device proof flags.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

import build_android_module as module
import build_boot_repack as repack
import build_boot_request as request
import build_e2fs_tools as e2fs
import build_resize_f2fs as resize
import build_storage as storage
import package_product_trampoline as trampoline

ROOT = Path(__file__).resolve().parents[1]
SYSROOT_PACKAGES = ('gcc', 'libc6-dev', 'uuid-dev', 'libblkid-dev')
STATIC_LIBRARIES = ('libc.a', 'libuuid.a', 'libblkid.a')


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def check_sysroot(root):
    missing = [name for name in STATIC_LIBRARIES
               if not (root / 'usr/lib/aarch64-linux-gnu' / name).is_file()]
    if not (root / 'usr/include/stdio.h').is_file():
        missing.append('libc headers')
    if not (root / 'usr/lib/gcc/aarch64-linux-gnu').is_dir():
        missing.append('ARM64 GCC support libraries')
    if missing:
        raise ValueError('ARM64 sysroot is missing: ' + ', '.join(missing))


def prepare_sysroot(root, suite, mirror, keyring):
    """Extract authenticated Debian packages without executing foreign code."""
    if os.geteuid():
        raise ValueError('Creating the ARM64 sysroot requires root; use the build container or --sysroot')
    if root.exists():
        raise ValueError('Use a fresh sysroot directory or pass an existing --sysroot')
    for name in ('debootstrap', 'dpkg-deb'):
        if not shutil.which(name):
            raise ValueError('Missing sysroot build dependency: ' + name)
    command = ['debootstrap', '--arch=arm64', '--download-only',
               '--variant=minbase', '--include=' + ','.join(SYSROOT_PACKAGES),
               '--force-check-sig', '--keyring=' + str(keyring), suite, str(root), mirror]
    subprocess.run(command, check=True)
    packages = sorted((root / 'var/cache/apt/archives').glob('*.deb'))
    if not packages:
        raise ValueError('debootstrap did not leave authenticated ARM64 packages')
    records = []
    # debootstrap --download-only has verified Release signatures and every
    # package checksum. Only unpack them: no chroot, binfmt, maintainer scripts
    # or changes to the build host package state are needed.
    for package in packages:
        info = subprocess.check_output(
            ['dpkg-deb', '-f', str(package), 'Package', 'Version', 'Architecture'], text=True)
        records.append({'file': package.name, 'sha256': sha(package), 'fields': info.strip()})
        subprocess.run(['dpkg-deb', '-x', str(package), str(root)], check=True)
    check_sysroot(root)
    record = {'architecture': 'arm64', 'suite': suite, 'mirror': mirror,
              'keyring_sha256': sha(keyring), 'signature_check': 'debootstrap --force-check-sig',
              'packages': records, 'foreign_code_executed': False}
    write_json(root.parent / 'sysroot-source.json', record)
    return record


def add_licenses_and_records(output, sysroot, records):
    """Ship notices and native build identities beside the existing libmd notice."""
    with zipfile.ZipFile(output, 'a', zipfile.ZIP_DEFLATED) as archive:
        for package in ('libc6', 'libblkid1', 'libuuid1'):
            notice = sysroot / 'usr/share/doc' / package / 'copyright'
            if not notice.is_file():
                raise ValueError('Sysroot is missing the static library copyright notice: ' + package)
            archive.writestr('licenses/' + package + '.copyright', notice.read_bytes())
        archive.writestr('native-build.json', json.dumps(records, indent=2) + '\n')
        # These project sources make the native helper rebuild commands visible
        # to a recipient of the ZIP, without embedding any proprietary input.
        for name in ('android/native/piano-boot-repack.c', 'android/native/piano-boot-request.c',
                     'android/native/piano-storage.c', 'android/native/piano-bluetooth-provision.c',
                     'android/native/piano-bluetooth-provision.h', 'android/native/jsmn.h',
                     'android/native/jsmn-source.json', 'tools/android/piano_resize_f2fs.c',
                     'uefi/handoff/bootselect/BootRequest.c', 'uefi/handoff/bootselect/BootRequest.h',
                     'tools/build_android_product_module.py', 'tools/build_boot_repack.py',
                     'tools/build_boot_request.py', 'tools/build_storage.py',
                     'tools/build_resize_f2fs.py', 'tools/build_e2fs_tools.py'):
            archive.writestr('sources/' + name, (ROOT / name).read_bytes())


def build(args):
    payloads, product = module.product_payload(args.product)
    version = next(line.partition('=')[2].strip() for line in
                   (ROOT / 'android/module/module.prop').read_text().splitlines()
                   if line.startswith('version='))
    if not re.fullmatch(r'[0-9A-Za-z.+_-]+', version):
        raise ValueError('Module version is not a safe artifact filename')
    work, artifacts = args.work.resolve(), args.output.resolve()
    output = artifacts / ('SunUEFI-Piano-' + version + '.zip')
    work.mkdir(parents=True, exist_ok=True)
    (work / '.incomplete').write_text('Android module build has not completed.\n')
    artifacts.mkdir(parents=True, exist_ok=True)
    sysroot = args.sysroot.resolve() if args.sysroot else work / 'sysroot'
    if args.sysroot:
        check_sysroot(sysroot)
        sysroot_record = {'architecture': 'arm64', 'provided_sysroot': True,
                          'libraries': {name: sha(sysroot / 'usr/lib/aarch64-linux-gnu' / name)
                                        for name in STATIC_LIBRARIES}}
    elif sysroot.exists():
        check_sysroot(sysroot)
        previous = sysroot.parent / 'sysroot-source.json'
        sysroot_record = module.read_json(previous) if previous.is_file() else {
            'architecture': 'arm64', 'reused_sysroot': True,
            'libraries': {name: sha(sysroot / 'usr/lib/aarch64-linux-gnu' / name)
                          for name in STATIC_LIBRARIES}}
    else:
        sysroot_record = prepare_sysroot(sysroot, args.suite, args.mirror, args.keyring.resolve())
    print('Building the early selector and static ARM64 Android tools.', flush=True)
    selector_dir = work / 'selector'
    if selector_dir.exists():
        shutil.rmtree(selector_dir)
    selector = trampoline.selector_only(selector_dir)
    payload_dir = work / 'payloads'
    payload_dir.mkdir(exist_ok=True)
    for name, data in payloads.items():
        (payload_dir / name).write_bytes(data)
    shutil.copy2(selector_dir / 'selector.bin', payload_dir / 'selector.bin')
    binary, native = repack.build(work / 'boot-repack', 'aarch64', args.cc, sysroot)
    repack.prototype_policy(binary, native, selector_dir, args.product.resolve(),
                            payload_dir / 'app.bin', work / 'boot-repack')
    policy_path = work / 'boot-repack/policy-prototype.json'
    policy = module.read_json(policy_path)
    descriptor = {'schema_version': 1, 'interface_version': 1, 'wrapper_version': 1,
                  'app_abi': 1, 'wrapper': 'SPLITv1+RSTRv1', 'stock_kernel_bundled': False,
                  'full_partition_backup': False, 'generic_core': True,
                  'selector': policy['selector'], 'payloads': policy['payloads']}
    write_json(payload_dir / 'payload-descriptor.json', descriptor)
    request_record = request.build(work / 'boot-request', 'aarch64', args.cc, sysroot)
    storage_record = storage.build(work / 'storage', 'aarch64', args.cc, sysroot)
    resize_record = resize.build(work / 'resize-f2fs', args.cc, sysroot, 'aarch64')
    print('Building static e2fsck and resize2fs from the fixed official source archive.', flush=True)
    e2fs_dir = work / 'e2fs-tools'
    e2fs_record = e2fs.build(e2fs_dir, sysroot, args.cache, args.jobs)
    # Keep the previous published ZIP usable until all new contents pass the
    # actual package checks. Rebuilding does not require a new directory name.
    pending_zip = artifacts / ('.' + output.name + '.part')
    pending_zip.unlink(missing_ok=True)
    package_args = module.parser().parse_args([
        '--installed-core', str(payload_dir), '--native-tool', str(binary),
        '--native-manifest', str(policy_path), '--request-tool', str(work / 'boot-request/piano-boot-request'),
        '--storage-tool', str(work / 'storage/piano-storage'),
        '--resize-tool', str(work / 'resize-f2fs/piano-resize-f2fs'),
        '--e2fs-tools', str(e2fs_dir), '--output', str(pending_zip)])
    result = module.package(package_args)
    records = {'schema_version': 1, 'artifact_kind': 'android-product-module', 'generic_core': True,
               'same_product': product, 'selector': selector, 'sysroot': sysroot_record,
               'boot_repack': native, 'boot_request': request_record, 'storage': storage_record,
               'resize_f2fs': resize_record, 'e2fsprogs': e2fs_record,
               'module_sources': {name: sha(ROOT / 'android/module' / name)
                                  for name in module.SOURCE_FILES},
               'device_operation_performed': False,
               'installation': 'Extract and retain the current active-slot Android GKI; adopt a matching core or upgrade an owned previous core',
               'ota': 'The helper can repack a supported current-ROM stock BOOT after OTA',
               'qualification': 'Build records do not mark unperformed native or device tests as passed'}
    with zipfile.ZipFile(pending_zip, 'a', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('licenses/COPYING.libfdt', (work / 'storage/COPYING.libfdt').read_bytes())
    add_licenses_and_records(pending_zip, sysroot, records)
    with zipfile.ZipFile(pending_zip) as archive:
        if archive.testzip() is not None:
            raise ValueError('Module ZIP readback failed')
        for name in ('piano-boot-repack', 'piano-boot-request', 'piano-storage',
                     'piano-resize-f2fs', 'e2fsck', 'resize2fs'):
            module.arm64_executable(archive.read('bin/' + name))
        for name in ('fd.bin', 'app.bin', 'shim.bin'):
            if archive.read('payload/' + name) != payloads[name]:
                raise ValueError('Module differs from the built UEFI product: ' + name)
    pending_zip.replace(output)
    result['zip'] = str(output)
    result.update({'artifact_kind': 'android-product-module', 'generic_core': True, 'zip_sha256': sha(output),
                   'zip_bytes': output.stat().st_size, 'same_product': product,
                   'device_operation_performed': False,
                   'installation': records['installation'],
                   'build_record': 'native-build.json'})
    write_json(artifacts / 'manifest.json', result)
    write_json(work / 'manifest.json', records)
    (artifacts / 'SHA256SUMS').write_text(sha(output) + '  ' + output.name + '\n')
    (work / '.incomplete').unlink()
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--product', type=Path, default=ROOT / 'artifacts/product')
    result.add_argument('--work', type=Path, default=ROOT / 'build/android-product-module')
    result.add_argument('--output', type=Path, default=ROOT / 'artifacts/android')
    result.add_argument('--sysroot', type=Path, help='Existing ARM64 libc/GCC/uuid/blkid development sysroot')
    result.add_argument('--cc', help='Native tool compiler; defaults to clang targeting ARM64')
    result.add_argument('--suite', default='trixie')
    result.add_argument('--mirror', default='https://deb.debian.org/debian')
    result.add_argument('--keyring', type=Path, default=Path('/usr/share/keyrings/debian-archive-keyring.gpg'))
    result.add_argument('--cache', type=Path, default=ROOT / 'build/source-cache/e2fsprogs')
    result.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    return result


if __name__ == '__main__':
    arguments = parser().parse_args()
    if arguments.jobs < 1:
        raise SystemExit('--jobs must be positive')
    try:
        print(json.dumps(build(arguments), indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
