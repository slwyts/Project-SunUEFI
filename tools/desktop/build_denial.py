#!/usr/bin/env python3
"""Build, test and package Denial from its signed upstream release."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build.denial import FINGERPRINT, KEY
from build.denial_manifest import TARGETS
from build_piano_runtime_helpers import source_patches
from desktop import denial_packages, package_build

CONFIG = ROOT / 'config/denial.json'


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def verify_source(source, policy, keyring):
    tag = policy['tag']
    commit = subprocess.check_output(
        ['git', '-C', source, 'rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}'],
        text=True).strip()
    if commit != policy['commit']:
        raise ValueError('The Denial release tag does not match the pinned commit')
    run(['gpg', '--no-options', '--batch', '--homedir', keyring, '--dearmor',
         '--output', keyring / 'pubring.gpg', KEY], capture_output=True)
    signature = subprocess.run(
        ['git', '-C', source, '-c', 'gpg.format=openpgp', '-c', 'gpg.program=gpg',
         'verify-tag', '--raw', f'refs/tags/{tag}'],
        env={**os.environ, 'GNUPGHOME': str(keyring)}, capture_output=True, text=True)
    keys = [line.split()[-1] for line in signature.stderr.splitlines()
            if line.startswith('[GNUPG:] VALIDSIG ')]
    if signature.returncode != 0 or keys != [FINGERPRINT]:
        raise ValueError('The Denial tag was not signed by the pinned release key')


def checkout_source(policy, output):
    run(['git', 'clone', '--quiet', '--depth=1', '--branch', policy['tag'],
         policy['repository'], output])
    return output


def build(args):
    policy = json.loads(CONFIG.read_text())['source']
    version = policy['tag'].removeprefix('v')
    patches = source_patches(ROOT / 'patches/denial' / version / 'series.json')
    output = args.output.resolve()
    if args.jobs < 1 or not output.is_relative_to(ROOT / 'build') or output == ROOT / 'build':
        raise ValueError('Use a project build directory and at least one job')
    if args.source and (output.is_relative_to(args.source.resolve()) or
                        args.source.resolve().is_relative_to(output)):
        raise ValueError('Use a build directory separate from the Denial checkout')
    host_arch = os.uname().machine
    if args.release and host_arch != 'aarch64':
        raise ValueError('Build on ARM64 or pass --arch-sysroot for an x86-64 cross-build')
    if args.arch_sysroot and host_arch != 'x86_64':
        raise ValueError('Use --release inside the ARM64 build environment')
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock, \
            tempfile.TemporaryDirectory(prefix='denial-source-', dir=output) as temporary:
        fcntl.flock(lock, fcntl.LOCK_EX)
        work = Path(temporary)
        upstream = args.source or checkout_source(policy, work / 'upstream')
        keyring = work / 'gnupg'
        keyring.mkdir(mode=0o700)
        verify_source(upstream, policy, keyring)
        archive = work / 'source.tar'
        run(['git', '-C', upstream, 'archive', '--format=tar', '-o', archive, policy['commit']])
        source = work / 'source'
        source.mkdir()
        run(['tar', '-xf', archive, '-C', source])
        run(['git', '-C', source, 'init', '--quiet'])
        run(['git', '-C', source, 'apply', '--whitespace=error', *patches])

        patch_records = {path.name: package_build.sha(path) for path in patches}
        patch_id = hashlib.sha256(json.dumps(patch_records).encode()).hexdigest()[:12]
        env = {**os.environ, 'CARGO_TARGET_DIR': str(output / 'target'),
               'DENIAL_BUILD_VERSION': f'{version}+sunuefi.{patch_id}'}
        options = ['--locked', '--features', 'flutter', '--bin', 'deniald',
                   '-j', str(args.jobs)]
        if args.test:
            run(['cargo', 'test', *options, 'orientation_sensor'], cwd=source / 'compositor', env=env)
            return
        command = '''
            source tools/lib/denial-architecture.sh
            denial_arch_configure aarch64
            denial_cross_configure_rust
            cd compositor
            if [[ -n "$DENIAL_RUST_TARGET" ]]; then
                set -- "$@" --target "$DENIAL_RUST_TARGET"
            fi
            exec cargo build --release "$@"
        '''
        binary = output / 'deniald'
        receipt = output / 'SOURCE.json'
        receipt.unlink(missing_ok=True)
        if args.arch_sysroot:
            env['DENIAL_ARM64_SYSROOT'] = str(args.arch_sysroot.resolve())
        run(['bash', '-euc', command, 'denial-build', *options], cwd=source, env=env)
        artifact = output / 'target'
        if args.arch_sysroot:
            artifact /= 'aarch64-unknown-linux-gnu'
        built = artifact / 'release/deniald'
        elf = subprocess.check_output(['readelf', '-h', '-d', built], text=True)
        if 'AArch64' not in elf or any(field in elf for field in ('(RPATH)', '(RUNPATH)')):
            raise ValueError('Expected an ARM64 Denial compositor linked to system libraries')
        shutil.copy2(built, binary)
        record = {'source': policy, 'patches': patch_records, 'binary_sha256': package_build.sha(binary)}
        receipt.write_text(json.dumps(record, indent=2) + '\n')
        print(binary)


def built_paths(output):
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build') or output == ROOT / 'build':
        raise ValueError('Use a Denial directory under build/')
    binary, receipt = output / 'deniald', output / 'SOURCE.json'
    if not receipt.is_file() or not binary.is_file():
        raise ValueError('Build the ARM64 compositor before packaging it')
    return binary, receipt


def verify_build(args):
    binary, receipt = built_paths(args.output)
    with (binary.parent / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH)
        record, _ = denial_packages.build_source(binary, receipt)
    print(f"Verified ARM64 Denial compositor SHA-256: {record['binary_sha256']}")


def package(args):
    binary, receipt = built_paths(args.output)
    destination = args.package_output.resolve()
    if not destination.is_relative_to(ROOT / 'artifacts/packages') or destination == ROOT / 'artifacts/packages':
        raise ValueError('Choose a new directory under artifacts/packages')
    target = args.overlay_target
    package_format = TARGETS[target][0] if target else args.package
    with (binary.parent / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH)
        if not target:
            denial_packages.build(binary, receipt, args.release_archives, destination, package_format,
                                  source=args.source, arch_root=args.arch_root)
        else:
            from build.denial_overlay import fetch_archives

            record, _ = denial_packages.build_source(binary, receipt)
            policy = record['source']
            archives = args.release_archives or fetch_archives(policy, package_format)
            with tempfile.TemporaryDirectory(prefix='denial-overlay-source-', dir=binary.parent) as work:
                source = args.source
                if package_format == 'pacman' and source is None:
                    source = checkout_source(policy, Path(work) / 'upstream')
                denial_packages.build(binary, receipt, archives, destination, package_format,
                                      source=source, arch_root=args.arch_root, target=target)
    print(destination)


def main():
    parser = argparse.ArgumentParser(prog='./build.sh denial', description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--release', action='store_true', help='Build on an ARM64 host or in an ARM64 container')
    mode.add_argument('--arch-sysroot', type=Path, help='ARM64 Arch sysroot for the release compositor')
    mode.add_argument('--test', action='store_true', help='Run the orientation tests on the build host')
    mode.add_argument('--verify-build', action='store_true', help='Verify a completed ARM64 build before reuse')
    mode.add_argument('--package', choices=('debian', 'pacman'), help='Package a completed ARM64 build')
    mode.add_argument('--overlay-target', choices=TARGETS,
                      help='Package the native software for the selected Denial image')
    parser.add_argument('--source', type=Path, help='Use an existing checkout with the signed release tag')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/denial', help='Compositor build directory')
    parser.add_argument('--release-archives', type=Path, help='Denial release packages, checksums and signatures')
    parser.add_argument('--package-output', type=Path, help='New directory for the native packages')
    parser.add_argument('--arch-root', type=Path, help='Isolated ARM64 Arch build root with makepkg')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs(2048))
    args = parser.parse_args()
    if args.package:
        if not args.release_archives or not args.package_output:
            parser.error('--package needs --release-archives and --package-output')
        if args.package == 'pacman' and (not args.source or not args.arch_root):
            parser.error('--package pacman needs --source and --arch-root')
        if args.package == 'debian' and args.arch_root:
            parser.error('--arch-root is only for --package pacman')
    elif args.overlay_target:
        if not args.package_output:
            parser.error('--overlay-target needs --package-output')
        if args.overlay_target == 'debian-denial' and args.arch_root:
            parser.error('--arch-root is only for the Arch Denial overlay')
    elif args.release_archives or args.package_output or args.arch_root:
        parser.error('--release-archives, --package-output and --arch-root are for packaging')
    try:
        if args.package or args.overlay_target:
            operation = package
        else:
            operation = verify_build if args.verify_build else build
        operation(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
