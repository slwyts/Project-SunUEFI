#!/usr/bin/env python3
"""Build the public runtime helpers with an explicit, recorded host toolchain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

import build_piano_runtime_helpers as runtime
from prepare_linux_modules import modinfo

ROOT = Path(__file__).resolve().parents[1]
STABLE_UAPI_COMMIT = '5fce161649b4d779d1b76d9fcd52dc77779774b8'
STABLE_UAPI_DIGEST = 'b0effcf9c1b646a06c6839a2b98e1c051a7854f23349f676cfb280d6b749b33e'


def expected_uapi_digest(source, commit):
    # v7.2.9 changes AMDGPU/RDMA headers and linux/version.h. The helpers'
    # input/media/V4L2 interfaces are unchanged; record the actual header tree.
    stable = subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor',
                             STABLE_UAPI_COMMIT, commit], capture_output=True)
    return STABLE_UAPI_DIGEST if stable.returncode == 0 else runtime.UAPI_DIGEST


def run(args, cwd=None):
    subprocess.run([str(x) for x in args], cwd=cwd, check=True)


def build(kernel, source, kernel_build, output, cc, sysroot, macros, loop):
    record = json.loads((kernel / 'manifest.json').read_text())
    release, commit = record['kernel_release'], record['source_commit']
    identity = runtime.kernel_identity(kernel_build, source, commit, release)
    compiler_source = runtime.kernel_compiler_source(kernel_build, source, commit)
    runtime.repository(ROOT / 'upstream/debian-piano-current', runtime.PUBLIC_COMMIT)
    runtime.repository(macros, runtime.MACROS_COMMIT)
    runtime.repository(loop, runtime.LOOP_COMMIT)
    if output.exists() or not output.resolve().is_relative_to(ROOT / 'build'):
        raise ValueError('Use a fresh project build output')
    compiler = shutil.which(cc)
    topology_compiler = shutil.which('alsatplg')
    if not compiler or not topology_compiler or not shutil.which('m4'):
        raise ValueError('Install the explicit compiler, alsatplg and m4 first')
    output.mkdir(parents=True)
    public = ROOT / 'upstream/debian-piano-current'
    uapi = output / 'uapi'
    run(['make', '-C', compiler_source, 'O=' + str(kernel_build), 'ARCH=arm64',
         'headers_install', 'INSTALL_HDR_PATH=' + str(uapi)])
    rows = runtime.tree_files(uapi / 'include')
    uapi_digest = expected_uapi_digest(source, commit)
    if runtime.tree_digest(rows) != uapi_digest:
        raise ValueError('Runtime UAPI differs from the reviewed public interfaces')
    flags = [compiler, '-O2', '-Wall', '-Wextra', '-Werror']
    if 'clang' in Path(compiler).name:
        flags += ['--target=aarch64-linux-gnu']
        if sysroot:
            flags += ['--sysroot=' + str(sysroot), '--gcc-toolchain=' + str(sysroot / 'usr')]
    if sysroot and 'clang' not in Path(compiler).name:
        flags += ['--sysroot=' + str(sysroot)]
    files = {}
    touch, touch_record = runtime.derive_touch_source(public, output)
    camera, camera_record = runtime.derive_camerad_source(public, output)
    native = platform.machine() in ('aarch64', 'arm64')
    emulator = None if native else shutil.which('qemu-aarch64-static') or shutil.which('qemu-aarch64')
    if not native and not emulator:
        raise ValueError('AArch64 helper checks need qemu-aarch64 on this host')
    for name, (relative, digest, destination) in (runtime.PUBLIC_SOURCES | runtime.BSP_SOURCES).items():
        original = (ROOT if name in runtime.BSP_SOURCES else public) / relative
        if name in runtime.BSP_SOURCES:
            digest = runtime.sha(original)
        elif runtime.sha(original) != digest:
            raise ValueError('Runtime helper source changed: ' + name)
        source_file = touch if name == 'piano-touch-view' else camera if name == 'piano-camerad' else original
        entry, obj, binary = output / (name + '-entry.c'), output / (name + '.o'), output / name
        entry.write_text(runtime.entry_source(name))
        run([*flags, '-isystem', uapi / 'include', '-Dmain=PianoOriginalMain',
             '-c', source_file, '-o', obj])
        run([*flags, '-static', entry, obj, '-lm', '-o', binary])
        item = runtime.verify_elf(binary)
        run(([emulator] if emulator else []) + [binary, '--help'])
        item.update(file=name, mode=0o755, source_sha256=digest, help_no_device_access=True)
        if name == 'piano-touch-view':
            item.update(touch_record)
        if name == 'piano-camerad':
            item.update(camera_record)
        if name in runtime.BSP_SOURCES:
            item.update(source_kind='project-bsp', source_path=relative)
        files[destination] = item
    run(['bash', public / 'scripts/build-topology.sh', macros, output / 'firmware'])
    topology = output / 'firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
    if runtime.sha(topology) != runtime.TOPOLOGY_PIN:
        raise ValueError('Topology bytes differ from the reviewed source build')
    run([topology_compiler, '-d', topology, '-o', output / 'topology-decoded.conf'])
    files['usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'] = {
        'file': str(topology.relative_to(output)), 'bytes': topology.stat().st_size,
        'sha256': runtime.sha(topology), 'mode': 0o644}
    module_source = output / 'v4l2loopback'
    module_source.mkdir()
    for name in subprocess.check_output(['git', '-C', str(loop), 'ls-files'], text=True).splitlines():
        target = module_source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(loop / name, target)
    run(['make', '-C', kernel_build, 'M=' + str(module_source), 'ARCH=arm64', 'LLVM=1', 'modules'])
    module = module_source / 'v4l2loopback.ko'
    item = modinfo(module)
    if not item['vermagic'].startswith(release + ' '):
        raise ValueError('External module ABI differs from the selected kernel')
    files['usr/lib/modules/' + release + '/updates/v4l2loopback.ko'] = {
        'file': str(module.relative_to(output)), 'bytes': module.stat().st_size,
        'sha256': runtime.sha(module), 'mode': 0o644, 'vermagic': item['vermagic'],
        'source_commit': runtime.LOOP_COMMIT}
    if runtime.kernel_identity(kernel_build, source, commit, release) != identity:
        raise ValueError('Kernel source/ABI changed during runtime helper build')
    result = {'status': 'RUNTIME_COMPILED_NOT_DEVICE_TESTED', 'kernel': identity,
        'public_commit': runtime.PUBLIC_COMMIT, 'runtime_files': files,
        'uapi_sha256': uapi_digest, 'uapi_source_commit': commit, 'macros_commit': runtime.MACROS_COMMIT,
        'v4l2_commit': runtime.LOOP_COMMIT, 'hardware_verified': False,
        'toolchain': {'compiler': compiler, 'compiler_sha256': runtime.sha(Path(compiler)),
            'version': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
            'alsatplg_sha256': runtime.sha(Path(topology_compiler))}}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('kernel', 'source', 'kernel-build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', default='clang' if platform.machine() in ('aarch64', 'arm64') else 'aarch64-linux-gnu-gcc')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--macros', type=Path, default=ROOT / 'upstream/audioreach-topology')
    parser.add_argument('--v4l2-source', type=Path, default=ROOT / 'upstream/v4l2loopback')
    args = parser.parse_args()
    result = build(args.kernel.resolve(), args.source.resolve(), args.kernel_build.resolve(),
                   args.output.resolve(), args.cc, args.sysroot, args.macros.resolve(), args.v4l2_source.resolve())
    print(json.dumps({'status': result['status'], 'kernel_release': result['kernel']['release']}))


if __name__ == '__main__':
    main()
