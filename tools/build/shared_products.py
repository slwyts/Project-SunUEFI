#!/usr/bin/env python3
"""Build or verify the firmware and kernel shared by every distribution."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build.cache import atomic_json, sha, source_identity

FIRMWARE_INPUTS = ('build.sh', 'Makefile', '.gitmodules', 'sources.lock.json',
                   'requirements-build.txt', 'tools', 'uefi', 'patches/firmware',
                   'patches/f2fs-tools', 'vendor/piano', 'config/piano-product.json',
                   'config/uefi-ext4.json', 'config/simpleinit.static.uefi.cfg', 'android',
                   ':(exclude,literal)tools/check_host.py',
                   ':(exclude,literal)tools/build/check_patch_series.py',
                   ':(exclude,literal)tools/package/85-piano-boot-space.install',
                   ':(exclude,literal)tools/package/boot.py',
                   ':(exclude,literal)tools/package/camera.py',
                   ':(exclude,literal)tools/build_release_rootfs.py',
                   ':(exclude,literal)tools/build_nyarch_rootfs.py',
                   ':(exclude,literal)tools/build_steamos_rootfs.py')
KERNEL_INPUTS = ('build.sh', 'sources.lock.json', 'config/release.json',
                'config/kernel-llvm.json', 'patches/linux', 'patches/v4l2loopback', 'linux/configs',
                'tools/prepare_release_kernel.py', 'tools/build/kernel.py',
                'tools/build_piano_full_kernel.py', 'tools/build_kernel.py',
                'tools/prepare_linux_modules.py', 'tools/stage_piano_kernel_modules.py',
                'tools/desktop/prepare_kernel_llvm.py', 'tools/desktop/package_build.py',
                'tools/package/kernel.py', 'tools/package/kernel-debian.mk',
                'tools/package/85-piano-boot-space.install',
                'tools/package/boot.py', 'tools/package/v4l2loopback.py', 'tools/package_release.py',
                'tools/compose_piano_dtb.py', 'tools/prepare_boot_files.py',
                'tools/build_piano_runtime_helpers.py', 'tools/build/overlay_packages.py',
                'vendor/piano-linux/board.dtb', 'linux/dts', 'config/keys')
KERNEL_BUILD_FILES = ('.config', 'Makefile', 'Module.symvers', 'include', 'scripts',
                      'arch/arm64/include/generated')


def environment_identity():
    image = os.environ.get('SUNUEFI_BUILD_ENVIRONMENT_ID')
    if image:
        return {'architecture': platform.machine(), 'container_image': image}
    # Local use has no immutable OCI image ID. Bind the cache to actual tools;
    # the native kernel and firmware validators still check their own inputs.
    names = ('python3', 'clang', 'lld-link', 'llvm-objcopy', 'make', 'iasl', 'nasm',
             'aarch64-linux-gnu-objcopy', 'aarch64-linux-gnu-nm', 'ccache')
    tools = {name: sha(Path(path).resolve()) for name in names
             if (path := shutil.which(name))}
    return {'architecture': platform.machine(), 'tools': tools}


def identity(kind, root=ROOT):
    root = Path(root).resolve()
    inputs = FIRMWARE_INPUTS if kind == 'firmware' else KERNEL_INPUTS
    rows = source_identity(root, (*inputs, 'tools/build/shared_products.py',
                                   'tools/build/cache.py', 'tools/build_container.py',
                                   'config/build-container.json',
                                   'containers/Dockerfile'))
    if kind == 'kernel':
        from build_piano_full_kernel import kernel_policy_identity
        rows['config/release.json'] = {'section': 'kernel',
                                       **kernel_policy_identity(root, with_packages=True)}
    record = {'schema_version': 1, 'kind': kind,
              'workspace': os.environ.get('SUNUEFI_CACHE_WORKSPACE', str(root)),
              'environment': environment_identity(), 'inputs': rows}
    digest = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()
    return record, digest


def output_paths(kind, root=ROOT):
    if kind == 'firmware':
        return [root / name for name in ('artifacts/product', 'artifacts/android',
                                         'artifacts/simpleinit/product')]
    return [root / 'artifacts/kernels/release', root / 'artifacts/packages/kernel',
            *[root / 'build/kernels/release' / name for name in KERNEL_BUILD_FILES]]


def inventory(paths, root):
    rows = {}
    for base in paths:
        if not base.exists():
            raise ValueError('Shared build output missing: ' + str(base))
        for path in ([base, *sorted(base.rglob('*'))] if base.is_dir() else [base]):
            name = path.relative_to(root).as_posix()
            if path.is_symlink():
                rows[name] = {'link': os.readlink(path)}
            elif path.is_file():
                rows[name] = {'sha256': sha(path), 'mode': path.stat().st_mode & 0o777}
    return rows


@contextmanager
def locked(kind, root=ROOT):
    directory = root / 'build/locks'
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / ('shared-products-' + kind + '.lock')).open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def verify_firmware(root=ROOT, source_build=False):
    import build_integrity
    import build_android_module
    import product_contract
    import simpleinit_build_identity
    from firmware_workspace import active_root

    lock = json.loads((root / 'sources.lock.json').read_text())
    for name, row in lock.items():
        if not name.startswith(('Mu-Silicium', 'simple-init')):
            continue
        repository = root / 'upstream' / name
        head = subprocess.check_output(['git', '-C', repository, 'rev-parse', 'HEAD'], text=True).strip()
        changes = subprocess.check_output(
            ['git', '-C', repository, 'status', '--porcelain=v1', '--untracked-files=all'], text=True)
        if head != row['commit'] or changes:
            raise ValueError('Shared firmware upstream source differs: ' + name)
    from piano_vendor_inputs import load
    load(root)

    folder = root / 'artifacts/product'
    manifest = build_android_module.read_json(folder / 'manifest.json')
    product_contract.validate_build_manifest(
        json.loads((root / 'config/piano-product.json').read_text()), manifest)
    marker = json.loads((folder / 'build-ok.json').read_text())
    if (marker.get('build_id') != manifest.get('build_id') or
            marker.get('inputs') != manifest.get('build_inputs')):
        raise ValueError('Shared firmware build marker differs from packaged inputs')
    for name, digest in marker.get('outputs', {}).items():
        if Path(name).name != name or sha(folder / name) != digest:
            raise ValueError('Shared firmware build output differs: ' + name)
    simpleinit = root / 'artifacts/simpleinit/product'
    app_identity = json.loads((simpleinit / 'build-ok.json').read_text())
    if (sha(simpleinit / 'SimpleInit.efi') != app_identity.get('application_sha256') or
            app_identity != manifest.get('simpleinit')):
        raise ValueError('Shared SimpleInit payload identity differs')
    if source_build:
        workspace = active_root(root)
        build_integrity.validate(root, 'product', manifest)
        built = simpleinit_build_identity.inspect(
            workspace / 'build/simpleinit-product-edk2', root / 'artifacts/simpleinit/product', True)
        if built != json.loads((root / 'artifacts/simpleinit/product/build-ok.json').read_text()):
            raise ValueError('Cached SimpleInit build identity differs')
    payloads, product = build_android_module.product_payload(folder)
    module = build_android_module.read_json(root / 'artifacts/android/manifest.json')
    filename = Path(module.get('zip', '')).name
    if not filename.startswith('SunUEFI-Piano-') or not filename.endswith('.zip'):
        raise ValueError('Shared Android module filename is missing')
    package = root / 'artifacts/android' / filename
    if not package.is_file() or sha(package) != module.get('zip_sha256'):
        raise ValueError('Shared Android module ZIP differs')
    if module.get('same_product') != product:
        raise ValueError('Shared Android module belongs to another firmware')
    with zipfile.ZipFile(package) as archive:
        if archive.testzip() is not None:
            raise ValueError('Shared Android module ZIP is corrupt')
        for name in ('fd.bin', 'app.bin', 'shim.bin'):
            if archive.read('payload/' + name) != payloads[name]:
                raise ValueError('Shared Android module payload differs: ' + name)
        for name in ('piano-boot-repack', 'piano-boot-request', 'piano-storage',
                     'piano-resize-f2fs', 'e2fsck', 'resize2fs', 'fsck.f2fs', 'resize.f2fs'):
            build_android_module.arm64_executable(archive.read('bin/' + name))
    return {'product_sha256': product['product_sha256'], 'module_sha256': sha(package)}


def verify_kernel(root=ROOT):
    from stage_piano_kernel_modules import inspect
    manifest, digest = inspect(root / 'artifacts/kernels/release')
    return {'kernel_release': manifest['kernel_release'], 'manifest_sha256': digest}


def check_kernel_hot_inputs(root=ROOT):
    """Refuse an incompatible restore before the normal builder can recompile."""
    import build_piano_full_kernel as kernel
    artifact = root / 'artifacts/kernels/release'
    record = json.loads((artifact / 'manifest.json').read_text())
    source = json.loads((root / 'build/release/source-manifest.json').read_text())
    if record.get('source_commit') != source.get('actual_commit'):
        raise ValueError('Shared kernel source differs from the selected release')
    if not kernel.kernel_policy_matches(record, kernel.kernel_policy_identity(root)):
        raise ValueError('Shared kernel policy differs or lacks section provenance; rebuild in the shared job')
    for name, digest in record['inputs'].items():
        if sha(root / name) != digest:
            raise ValueError('Shared kernel build input differs: ' + name)
    _, tools = kernel.toolchain()
    if tools['sha256'] != record['toolchain']['sha256']:
        raise ValueError('Shared kernel toolchain differs; rebuild in the shared job')
    if (record.get('root_policy') != 'LABEL=PIANOROOT' or
            record.get('command_line') != kernel.command_line('LABEL=PIANOROOT')):
        raise ValueError('Shared kernel boot policy differs')
    out = root / 'build/kernels/release'
    if (sha(out / '.config') != record.get('config_sha256') or
            sha(out / 'Module.symvers') != record.get('module_symvers_sha256')):
        raise ValueError('Shared kernel configuration or Module.symvers differs')
    fragments = (('piano-bluetooth.config', 'bluetooth'), ('piano-filesystem.config', 'filesystem'),
                 ('piano-gamepad.config', 'gamepad'),
                 ('piano-pen-dock.config', 'pen-dock'), ('piano-ir.config', 'ir'),
                 ('piano-flash.config', 'flash'), ('piano-cpucp-early.config', 'early-cpucp'),
                 ('piano-userspace-security.config', 'userspace-security'),
                 ('piano-sleep.config', 'sleep'), ('piano-platform.config', 'platform'))
    overrides = {}
    for name, kind in fragments:
        overrides.update(kernel.module_overrides((root / 'linux/configs' / name).read_text(), kind))
    if record.get('local_module_overrides') != overrides:
        raise ValueError('Shared kernel configuration overrides differ')
    compiler = root / 'build/kernel-worktrees' / (
        'release-compiler-' + hashlib.sha256(str(out).encode()).hexdigest()[:12])
    expected = ["# Automatically generated by " + str(compiler) + "/Makefile: don't edit",
                'export KBUILD_OUTPUT = ' + str(out), 'include ' + str(compiler) + '/Makefile']
    if (out / 'Makefile').read_text().splitlines() != expected:
        raise ValueError('Shared kernel build Makefile belongs to another workspace')
    if not kernel.kernel_release_matches(record, out):
        raise ValueError('Shared external-module kernel release differs')
    kernel.verify_source(Path(source['worktree']), record['source_commit'])


def prepare_kernel_sources(root=ROOT):
    repository = root / 'build/kernel-sources/linux-piano.git'
    if repository.is_dir():
        subprocess.run(['git', '-C', repository, 'worktree', 'prune', '--expire=now'], check=True)
    subprocess.run([sys.executable, root / 'tools/prepare_release_kernel.py', '--refresh'],
                   cwd=root, check=True)


def run(kind, verify_only=False, root=ROOT):
    root = Path(root).resolve()
    started = time.monotonic()
    with locked(kind, root):
        record, key = identity(kind, root)
        receipt = root / 'build/shared-products' / (kind + '.json')
        cached = json.loads(receipt.read_text()) if receipt.is_file() else None
        try:
            matching = (cached is not None and cached.get('identity') == record and
                        cached.get('outputs') == inventory(output_paths(kind, root), root))
        except (OSError, ValueError):
            matching = False
        if verify_only and not matching:
            raise ValueError('Shared ' + kind + ' cache is missing, stale or modified')
        if not matching:
            # Only a validated generation gets a receipt; a failed normal build
            # cannot leave the previous result eligible for cache publication.
            receipt.unlink(missing_ok=True)
        if matching and not cached.get('verification'):
            raise ValueError('Shared cache has no successful build verification')
        if kind == 'kernel':
            # This native entry checks source cleanliness, compiler hashes,
            # config, Module.symvers and every module before its hot return.
            prepare_kernel_sources(root)
            if matching:
                # Let the native entry recreate its compiler checkout while
                # holding its own source/output locks, then take its hot path.
                check_kernel_hot_inputs(root)
            subprocess.run([root / 'build.sh', 'linux'], cwd=root, check=True)
            result = verify_kernel(root)
            if not matching:
                for package_format in ('debian', 'pacman'):
                    output = root / 'artifacts/packages/kernel' / package_format
                    if output.exists():
                        shutil.rmtree(output)
                    subprocess.run([root / 'build.sh', 'linux-package', '--format', package_format,
                                    '--output', output], cwd=root, check=True)
        elif matching:
            result = verify_firmware(root)
        else:
            import prepare_sources
            prepare_sources.check(root, 'firmware')
            subprocess.run([root / 'build.sh', 'uefi'], cwd=root, check=True)
            subprocess.run([root / 'build.sh', 'module'], cwd=root, check=True)
            result = verify_firmware(root, source_build=True)
        if identity(kind, root)[0] != record:
            raise ValueError('Shared build inputs changed during ' + kind)
        outputs = inventory(output_paths(kind, root), root)
        if matching and outputs != cached['outputs']:
            raise ValueError('Shared cache changed during verification')
        if matching and result != cached['verification']:
            raise ValueError('Shared cache build verification changed')
        if not matching:
            atomic_json(receipt, {'schema_version': 1, 'identity': record,
                                  'outputs': outputs, 'verification': result})
    print(json.dumps({'kind': kind, 'cache_key': key, 'reused': matching,
                      'seconds': round(time.monotonic() - started, 3),
                      'verification': result}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('keys', 'ensure', 'verify'))
    parser.add_argument('--kind', choices=('firmware', 'kernel'))
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args()
    if args.operation == 'keys':
        values = {kind + '_key': 'shared-' + kind + '-' + identity(kind)[1]
                  for kind in ('firmware', 'kernel')}
        if args.github_output:
            with args.github_output.open('a') as stream:
                stream.write(''.join(name + '=' + value + '\n' for name, value in values.items()))
        else:
            print(json.dumps(values))
    else:
        if args.kind is None:
            parser.error('--kind is required for ensure and verify')
        run(args.kind, args.operation == 'verify')


if __name__ == '__main__':
    main()
