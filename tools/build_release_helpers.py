#!/usr/bin/env python3
"""Build the public runtime helpers with an explicit, recorded host toolchain."""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess

import build_piano_runtime_helpers as runtime
import build_piano_fingerprint
from prepare_release_kernel import release_series

ROOT = Path(__file__).resolve().parents[1]
STABLE_UAPI_COMMIT = '5fce161649b4d779d1b76d9fcd52dc77779774b8'
STABLE_UAPI_DIGEST = 'b0effcf9c1b646a06c6839a2b98e1c051a7854f23349f676cfb280d6b749b33e'
LINUX_73_COMMIT = 'c03c1a14507dfaf8b856c2608f9ac216021fc222'
LINUX_73_UAPI_DIGEST = '12ed203c6e7e5dd78106fb7a9127dd620a6ae90272a8a627c05cf6ad43750591'

def inputs_current(output):
    path = output / 'manifest.json'
    if not path.is_file():
        return False
    record = json.loads(path.read_text())
    pen = record.get('pen_core', {})
    if pen.get('input', {}).get('mode') != 'kernel':
        return False
    pen_source = ROOT / 'linux/userspace/pen'
    pen_paths = [Path(runtime.__file__),
                 *[pen_source / name for name in runtime.PEN_SOURCE_FILES],
                 pen_source / 'piano-pen-bluetooth.py', pen_source / 'piano-pen-canvas.py',
                 pen_source / 'piano-pen-control',
                 *[ROOT / name for name in runtime.PEN_CALIBRATION_PATHS.values()]]
    return (
        record.get('producer', {}).get('sha256') == runtime.sha(Path(__file__))
        and all(pen.get('inputs', {}).get(str(path)) == runtime.sha(path)
                for path in pen_paths)
        and record.get('camera', {}).get('inputs') == runtime.camera_inputs()
        and record.get('fingerprint', {}).get('inputs')
        == build_piano_fingerprint.source_inputs()
    )


def expected_uapi_digest(source, commit):
    policy = json.loads((ROOT / 'config/release.json').read_text())['kernel']
    series = release_series(ROOT, policy)
    release = subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor',
                              series['base_commit'], commit], capture_output=True)
    if release.returncode == 0 and policy.get('uapi_sha256'):
        return policy['uapi_sha256']
    current = subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor',
                              LINUX_73_COMMIT, commit], capture_output=True)
    if current.returncode == 0:
        return LINUX_73_UAPI_DIGEST
    # v7.2.9 changes AMDGPU/RDMA headers and linux/version.h. The helpers'
    # input/media/V4L2 interfaces are unchanged; record the actual header tree.
    stable = subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor',
                             STABLE_UAPI_COMMIT, commit], capture_output=True)
    return STABLE_UAPI_DIGEST if stable.returncode == 0 else runtime.UAPI_DIGEST


def run(args, cwd=None):
    subprocess.run([str(x) for x in args], cwd=cwd, check=True)


def export_uapi(source, kernel_build, commit, output):
    compiler_source = runtime.kernel_compiler_source(kernel_build, source, commit)
    run(['make', '-C', compiler_source, 'O=' + str(kernel_build), 'ARCH=arm64',
         'headers_install', 'INSTALL_HDR_PATH=' + str(output)])
    expected = expected_uapi_digest(source, commit)
    if runtime.tree_digest(runtime.tree_files(output / 'include')) != expected:
        raise ValueError('Runtime UAPI differs from the reviewed public interfaces')
    return expected


def build(kernel, source, kernel_build, output, cc, sysroot, macros, firmware_root=None):
    record = json.loads((kernel / 'manifest.json').read_text())
    release, commit = record['kernel_release'], record['source_commit']
    identity = runtime.kernel_identity(kernel_build, source, commit, release)
    runtime.repository(ROOT / 'upstream/debian-piano-current', runtime.PUBLIC_COMMIT)
    runtime.repository(macros, runtime.MACROS_COMMIT)
    if output.exists() or not output.resolve().is_relative_to(ROOT / 'build'):
        raise ValueError('Use a fresh project build output')
    compiler = shutil.which(cc)
    topology_compiler = shutil.which('alsatplg')
    if not compiler or not topology_compiler or not shutil.which('m4'):
        raise ValueError('Install the explicit compiler, alsatplg and m4 first')
    output.mkdir(parents=True)
    public = ROOT / 'upstream/debian-piano-current'
    uapi = output / 'uapi'
    uapi_digest = export_uapi(source, kernel_build, commit, uapi)
    flags = [compiler, '-O2', '-Wall', '-Wextra', '-Werror']
    if 'clang' in Path(compiler).name:
        flags += ['--target=aarch64-linux-gnu']
        if sysroot:
            flags += ['--sysroot=' + str(sysroot), '--gcc-toolchain=' + str(sysroot / 'usr')]
    if sysroot and 'clang' not in Path(compiler).name:
        flags += ['--sysroot=' + str(sysroot)]
    files = {}
    touch, touch_record = runtime.derive_touch_source(public, output)
    native = platform.machine() in ('aarch64', 'arm64')
    emulator = None if native else shutil.which('qemu-aarch64-static') or shutil.which('qemu-aarch64')
    if not native and not emulator:
        raise ValueError('AArch64 helper checks need qemu-aarch64 on this host')
    pen_files, pen_record = runtime.build_pen_core(
        output, flags, sysroot, emulator=emulator, kernel=identity,
        firmware_root=firmware_root)
    files.update(pen_files)
    camera_files, camera_record = runtime.build_camera(
        output, flags, pen_record['cxx_command'], uapi, sysroot, emulator=emulator)
    files.update(camera_files)
    for name, (relative, digest, destination) in runtime.PUBLIC_SOURCES.items():
        original = public / relative
        if runtime.sha(original) != digest:
            raise ValueError('Runtime helper source changed: ' + name)
        source_file = touch if name == 'piano-touch-view' else original
        entry, obj, binary = output / (name + '-entry.c'), output / (name + '.o'), output / name
        entry.write_text(runtime.entry_source(name))
        if name == 'piano-touch-view':
            item = runtime.build_touch_binary(output, flags, uapi, source_file, pen_record)
        else:
            run([*flags, '-isystem', uapi / 'include', '-Dmain=PianoOriginalMain',
                 '-c', source_file, '-o', obj])
            run([*flags, '-static', entry, obj, '-lm', '-o', binary])
            item = runtime.verify_elf(binary)
        run(([emulator] if emulator else []) + [binary, '--help'])
        item.update(file=name, mode=0o755, source_sha256=digest, help_no_device_access=True)
        if name == 'piano-touch-view':
            item.update(touch_record)
        files[destination] = item
    fingerprint_files, fingerprint = build_piano_fingerprint.build(
        output, flags, uapi, emulator=emulator)
    fingerprint_binary = output / fingerprint_files[
        'usr/local/sbin/piano-fingerprint-ctl']['file']
    fingerprint_files['usr/local/sbin/piano-fingerprint-ctl'].update(
        runtime.verify_elf(fingerprint_binary), help_no_device_access=True)
    files.update(fingerprint_files)
    runtime.build_topology(public, macros, output / 'firmware')
    topology = output / 'firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'
    if runtime.sha(topology) != runtime.TOPOLOGY_PIN:
        raise ValueError('Topology bytes differ from the reviewed source build')
    run([topology_compiler, '-d', topology, '-o', output / 'topology-decoded.conf'])
    files['usr/lib/firmware/qcom/sm8750/Xiaomi Pad 8 Pro-tplg.bin'] = {
        'file': str(topology.relative_to(output)), 'bytes': topology.stat().st_size,
        'sha256': runtime.sha(topology), 'mode': 0o644}
    if runtime.kernel_identity(kernel_build, source, commit, release) != identity:
        raise ValueError('Kernel source/ABI changed during runtime helper build')
    result = {'status': 'RUNTIME_COMPILED_NOT_DEVICE_TESTED', 'kernel': identity,
        'public_commit': runtime.PUBLIC_COMMIT, 'runtime_files': files, 'pen_core': pen_record, 'camera': camera_record,
        'producer': {'file': 'tools/build_release_helpers.py', 'sha256': runtime.sha(Path(__file__))},
        'fingerprint': fingerprint,
        'uapi_sha256': uapi_digest, 'uapi_source_commit': commit,
        'macros_commit': runtime.MACROS_COMMIT, 'hardware_verified': False,
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
    parser.add_argument('--firmware-root', type=Path,
                        help='Target root containing pen firmware calibration; defaults to --sysroot')
    parser.add_argument('--macros', type=Path, default=ROOT / 'upstream/audioreach-topology')
    args = parser.parse_args()
    result = build(args.kernel.resolve(), args.source.resolve(), args.kernel_build.resolve(),
                   args.output.resolve(), args.cc, args.sysroot.resolve() if args.sysroot else None,
                   args.macros.resolve(),
                   firmware_root=args.firmware_root.resolve() if args.firmware_root else None)
    print(json.dumps({'status': result['status'], 'kernel_release': result['kernel']['release']}))


if __name__ == '__main__':
    main()
