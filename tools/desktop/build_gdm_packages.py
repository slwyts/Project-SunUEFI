#!/usr/bin/env python3
"""Build Debian or Arch GDM with KMS readiness and reauthentication cleanup."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_gsd_packages as debian
import build_arch_package
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build

SERIES = ROOT / 'patches/gdm/51.0/series.json'
RUNTIME = {'gdm3': 'arm64', 'libgdm1': 'arm64', 'gir1.2-gdm-1.0': 'arm64'}
REVISION = 2


def selection():
    # The Xsession/ssh-agent change belongs to Arch packaging, not Debian.
    return [path for path in source_patches(SERIES) if path.name != '0002-Xsession-no-ssh-agent.patch']


def target_selection(rootfs):
    target = debian_package.target_identity(rootfs, 'gdm3')
    if target['binary']['source_version'].split('-', 1)[0] not in json.loads(SERIES.read_text())['versions']:
        raise ValueError('The shared GDM patch requires the signed sid GDM 51 source')
    return target


def reusable(output, rootfs):
    patches = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian.sha(path)} for path in selection()]
    inputs = debian_package.input_records([Path(__file__), SERIES, Path(debian_package.__file__),
                                          Path(package_build.__file__)])
    return debian_package.reusable(Path(output), target_selection(rootfs), patches, RUNTIME, inputs, REVISION)


def build(output, sysroot, jobs, builder_root=None, install_dependencies=True):
    target_selection(sysroot or builder_root)
    patches = selection()
    if len(patches) != 2:
        raise ValueError('The shared GDM series must contain its KMS readiness and reauthentication patches')
    return debian_package.build(output, sysroot, jobs, 'gdm3', patches, RUNTIME, [Path(__file__), SERIES],
                                revision=REVISION, builder_root=builder_root,
                                install_dependencies=install_dependencies)


def build_arch(root, output):
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output)):
        raise ValueError('Use root and separate project Arch rootfs and output directories')
    installed = debian.capture(['chroot', root, 'pacman', '-Q', 'gdm']).split()[1]
    version = installed.rsplit('-', 1)[0]
    series = ROOT / 'patches/gdm' / version / 'series.json'
    data = json.loads(series.read_text())
    patches = source_patches(series)
    template = ROOT / 'linux/desktops/gnome/arch/gdm/PKGBUILD'
    inputs = {path.relative_to(ROOT).as_posix(): debian.sha(path)
              for path in (series, *patches, template, Path(__file__))}
    metadata = output / 'SOURCE.json'
    if metadata.is_file():
        record = json.loads(metadata.read_text())
        packages = record.get('packages', {})
        if (record.get('inputs') == inputs and packages and
                build_arch_package.matches_release(output / 'runtime') and
                all(Path(name).name == name and (output / 'runtime' / name).is_file() and
                    debian.sha(output / 'runtime' / name) == row['sha256']
                    for name, row in packages.items())):
            build_arch_package.install_cached(root, output / 'runtime')
            return record
    output.mkdir(parents=True, exist_ok=True)
    recipe = output / 'recipe'
    recipe.mkdir(exist_ok=True)
    for patch in patches:
        shutil.copy2(patch, recipe / patch.name)
    sources = [data['source']['url'], *[path.name for path in patches]]
    checksums = [data['source']['sha256'], *[debian.sha(path) for path in patches]]
    text = template.read_text().replace('@VERSION@', data['version'])
    text = text.replace('@RELEASE@', data['package_release'])
    text = text.replace('@SOURCES@', ' '.join(map(shlex.quote, sources)))
    text = text.replace('@CHECKSUMS@', ' '.join(map(shlex.quote, checksums)))
    (recipe / 'PKGBUILD').write_text(text)
    runtime = output / 'runtime'
    with (output / 'build.log').open('a') as log:
        subprocess.run([sys.executable, ROOT / 'tools/build_arch_package.py', '--rootfs', root,
                        '--recipe', recipe, '--output', runtime],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    record = json.loads((runtime / 'manifest.json').read_text())
    record.update(inputs=inputs, source=data['source'])
    metadata.write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--sysroot', type=Path, help='Debian ARM64 input; cloned before use')
    mode.add_argument('--builder-root', type=Path, help='Existing isolated sid ARM64 package builder')
    mode.add_argument('--rootfs', type=Path, help='Arch ARM64 target; installs the built packages')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/gdm')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--skip-install-dependencies', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    if args.rootfs:
        print(json.dumps(build_arch(args.rootfs, args.output), indent=2))
    else:
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.builder_root,
                               not args.skip_install_dependencies), indent=2))
