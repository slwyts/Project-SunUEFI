#!/usr/bin/env python3
"""Build the libcamera CAMSS pipeline with Debian's ARM64 package recipe."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian, overlay_release
from build_piano_runtime_helpers import source_patches
from desktop import debian_package as signed, package_build
from prepare_steamos_rootfs import download

CONFIG = ROOT / 'config/libcamera.json'
PACKAGE = 'libcamera'
PROFILES = 'nocheck nodoc'


def policy():
    return json.loads(CONFIG.read_text())


def upstream_version(data):
    return data['source']['tag'].removeprefix('v')


def abi_version(data):
    return upstream_version(data).rsplit('.', 1)[0]


def runtime_packages(data):
    abi = abi_version(data)
    return dict.fromkeys((
        f'libcamera{abi}', f'libcamera-ipa-{abi}', 'libcamera-dev', 'libcamera-tools',
        'libcamera-v4l2', 'gstreamer1.0-libcamera', 'python3-libcamera',
    ), 'arm64')


def base_package_version(data):
    return upstream_version(data) + '-' + data['debian']['package_revision']


def patch_inputs(data):
    series = ROOT / 'patches/libcamera' / upstream_version(data) / 'series.json'
    patches = source_patches(series)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': package_build.sha(path)}
             for path in patches]
    return patches, proof


def target_identity(root, data):
    recipe_version = data['debian']['recipe_source_version']
    return debian.target_identity(root, {PACKAGE: recipe_version}, {PACKAGE: 'libcamera-dev'})


def verify_debian_packages(record, *, data=None):
    data = policy() if data is None else data
    version = record.get('package_version', '')
    packages = record.get('packages', {})
    runtime = runtime_packages(data)
    if (overlay_release.base_debian_version(version) != base_package_version(data) or
            set(packages) != set(runtime) or
            any(row.get('version') != version or row.get('architecture') != runtime[name]
                for name, row in packages.items())):
        raise ValueError('The Debian libcamera set must contain all native packages at the same version')


def reusable(output, root):
    data = policy()
    _, patches = patch_inputs(data)
    return cached_packages(Path(output), target_identity(root, data), data, patches)


def cached_packages(output, target, data, patches):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        verify_debian_packages(record, data=data)
        if (record.get('official_source') != data['source'] or
                record.get('config_sha256') != package_build.sha(CONFIG) or
                record.get('producer') != Path(__file__).relative_to(ROOT).as_posix()):
            return None
        version = overlay_release.debian_version(base_package_version(data))
        return debian.reusable(output, version, patches, runtime_packages(data), target)
    except (KeyError, OSError, ValueError):
        return None


def prepare_ipa_package(packaging, control, abi):
    """Keep the matching IPA optional for users of pipelines that do not need it."""
    old_name = 'libcamera-ipa'
    name = f'{old_name}-{abi}'
    stanzas = control.replace(old_name, name).split('\n\n')
    for index, stanza in enumerate(stanzas):
        if stanza.startswith(f'Package: {name}\n'):
            dependency = f'libcamera{abi} (= ${{binary:Version}})'
            stanzas[index] = stanza.replace('\nDepends: ', f'\nDepends: {dependency},\n         ', 1)
            break
    else:
        raise ValueError('The signed Debian recipe does not declare the IPA package')

    install = packaging / (name + '.install')
    (packaging / (old_name + '.install')).rename(install)
    install.write_text(f'usr/lib/*/libcamera/{abi}/ipa\n'
                       f'usr/libexec/*/libcamera/{abi}/*_ipa_proxy\n'
                       f'usr/share/libcamera/{abi}\n')
    rules = packaging / 'rules'
    rules.write_text(rules.read_text()
                     .replace(f'debian/{old_name}/', f'debian/{name}/')
                     .replace('/libcamera/ipa/ipa_*.so', f'/libcamera/{abi}/ipa/ipa_*.so'))
    return '\n\n'.join(stanzas)


def prepare_packaging(source, data, patches, version):
    """Retarget the signed Debian recipe to the source ABI and build requirements."""
    old = data['debian']['recipe_source_version'].split('-', 1)[0].rsplit('.', 1)[0]
    new = abi_version(data)
    packaging = source / 'debian'
    control = packaging / 'control'
    original = control.read_text()
    old_name, new_name = PACKAGE + old, PACKAGE + new
    if f'\nPackage: {old_name}\n' not in original or new_name in original:
        raise ValueError('The signed Debian recipe no longer uses the expected libcamera ABI')
    meson = re.search(r"(?m)^\s*meson_version\s*:\s*'(>= [\d.]+)'", (source / 'meson.build').read_text())
    if not meson:
        raise ValueError('The upstream libcamera source does not state its Meson requirement')
    updated, requirements = re.subn(r'\bmeson \(>= [\d.]+\)', f'meson ({meson[1]})', original)
    if requirements != 1:
        raise ValueError('The signed Debian recipe must declare one Meson requirement')
    control.write_text(prepare_ipa_package(packaging, updated.replace(old_name, new_name), new))
    (packaging / (old_name + '.install')).rename(packaging / (new_name + '.install'))
    debian.add_patches(source, patches, PACKAGE, version,
                      'Enable CAMSS online TFE PIX and keep IPA files separate by ABI.')


def prepare_source(directory, archive, recipe, data, patches, version):
    with tarfile.open(archive) as original:
        original.extractall(directory, filter='data')
    source = directory / f'libcamera-{data["source"]["commit"]}'
    if not (source / 'meson.build').is_file() or (source / 'debian').exists():
        raise ValueError('Unexpected libcamera upstream source layout')
    with tarfile.open(recipe) as packaging:
        packaging.extractall(source, filter='data')
    prepare_packaging(source, data, patches, version)
    shutil.copyfile(archive, directory / f'libcamera_{upstream_version(data)}.orig.tar.gz')
    return source


def verify_runtime(output, version, packages, data):
    abi = abi_version(data)
    library_package, ipa_package = f'libcamera{abi}', f'libcamera-ipa-{abi}'

    def dynamic(path):
        return debian.capture(['readelf', '--dynamic', '--wide', path])

    runtime = output / 'runtime'
    with tempfile.TemporaryDirectory(prefix='verify-', dir=output) as temporary:
        root = Path(temporary)
        ipa_root = root / 'ipa'
        for name, row in packages.items():
            debian.run(['dpkg-deb', '-x', runtime / row['file'], ipa_root if name == ipa_package else root])
        library = root / 'usr/lib/aarch64-linux-gnu'
        for name in (f'libcamera.so.{abi}', f'libcamera-base.so.{abi}'):
            if not any('(SONAME)' in line and f'[{name}]' in line
                       for line in dynamic(library / name).splitlines()):
                raise ValueError('Unexpected libcamera SONAME: ' + name)
        gst = dynamic(library / 'gstreamer-1.0/libgstlibcamera.so')
        if not any('(NEEDED)' in line and f'[libcamera.so.{abi}]' in line for line in gst.splitlines()):
            raise ValueError('The GStreamer plugin does not link the new libcamera ABI')
        ipa_library = ipa_root / 'usr/lib/aarch64-linux-gnu/libcamera'
        ipa_exec = ipa_root / 'usr/libexec/aarch64-linux-gnu/libcamera'
        ipa_data = ipa_root / 'usr/share/libcamera'
        modules = list((ipa_library / abi / 'ipa').glob('ipa_*.so'))
        if not modules or any(not module.with_suffix('.so.sign').is_file() for module in modules):
            raise ValueError('A libcamera IPA module does not have its Debian build signature')
        if (not list((ipa_exec / abi).glob('*_ipa_proxy')) or
                not all((ipa_data / abi / name).is_dir() for name in ('ipa', 'pipeline')) or
                any(path.exists() for path in (ipa_library / 'ipa', ipa_data / 'ipa', ipa_data / 'pipeline')) or
                any(ipa_exec.glob('*_ipa_proxy'))):
            raise ValueError('The Debian IPA package must install its private files by libcamera ABI')
    requirements = ((library_package, 'Recommends', f'{ipa_package} (= {version})'),
                    (ipa_package, 'Depends', f'{library_package} (= {version})'),
                    ('libcamera-dev', 'Depends', f'{library_package} (= {version})'))
    for name, field, relation in requirements:
        fields = debian.capture(['dpkg-deb', '-f', runtime / packages[name]['file'], field])
        if relation not in [item.strip() for item in fields.split(',')]:
            raise ValueError(f'{name} does not select its matching {relation}')


def build(output, sysroot, jobs=None):
    output, sysroot = Path(output).resolve(), Path(sysroot).resolve()
    jobs = package_build.default_jobs() if jobs is None else jobs
    if (os.geteuid() or not output.is_relative_to(ROOT / 'build') or not sysroot.is_relative_to(ROOT / 'build') or
            output.is_relative_to(sysroot) or sysroot.is_relative_to(output) or jobs < 1):
        raise ValueError('Use root, separate project build directories, and at least one build job')
    data = policy()
    patches, proof = patch_inputs(data)
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        target = target_identity(sysroot, data)
        if record := cached_packages(output, target, data, proof):
            return record
        return build_packages(output, sysroot, jobs, data, target, patches, proof)


def build_packages(output, sysroot, jobs, data, target, patches, proof):
    incomplete = output / '.incomplete'
    incomplete.write_text('The Debian libcamera package build has not completed.\n')
    version = overlay_release.debian_version(base_package_version(data))
    recipe_version = data['debian']['recipe_source_version']
    cache = ROOT / 'build/source-cache/libcamera'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / (data['source']['sha256'] + '.tar.gz')
    with (cache / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        download(data['source']['url'], archive, data['source']['sha256'])
    with (signed.use_build_archive(signed.archive_inputs(sysroot)),
          debian.builder(output, sysroot) as (root, env),
          (output / 'build.log').open('a') as log):
        debian.dependencies(root, log)
        _, origin = debian.source(root, output, PACKAGE, recipe_version, log)
        packaging = next(row['file'] for row in origin['archives'] if '.debian.tar.' in row['file'])
        recipe = output / 'archives' / PACKAGE / packaging
        debian.guest(root, ['apt-get', 'build-dep', '-y', '--no-install-recommends', '--no-fix-missing',
                            '--arch-only', '--build-profiles=nocheck,nodoc', PACKAGE + '=' + recipe_version],
                     stdout=log, stderr=subprocess.STDOUT)
        debian.guest(root, ['apt-get', 'install', '-y', '--no-install-recommends', 'libgtest-dev'],
                     stdout=log, stderr=subprocess.STDOUT)
        with tempfile.TemporaryDirectory(prefix='package-', dir=output) as temporary:
            directory = Path(temporary)
            source = prepare_source(directory, archive, recipe, data, patches, version)
            debian.command(root, {}, source, ['dpkg-source', '-b', '.'], log)
            build_command = ['dpkg-buildpackage', '-B', '-us', '-uc', '-j' + str(jobs)]
            flags = dict(env, DEB_BUILD_PROFILES=PROFILES, DEB_BUILD_OPTIONS=PROFILES + f' parallel={jobs}')
            debian.command(root, flags, source, build_command, log)
            packages = debian.collect(output, directory, runtime_packages(data), version)
            verify_runtime(output, version, packages, data)
            runtime = output / 'runtime'
            files = {row['file'] for row in packages.values()}
            for path in runtime.glob('*.deb'):
                if path.name not in files:
                    path.unlink()
            receipts = output / 'source'
            receipts.mkdir(exist_ok=True)
            for path in directory.iterdir():
                if path.is_file() and path.suffix != '.deb':
                    shutil.copy2(path, receipts / path.name)
            record = {'package_version': version, 'packages_built': True, 'packages': packages,
                      'target': dict(target), 'patches': proof, 'source': origin, 'official_source': data['source'],
                      'producer': str(Path(__file__).relative_to(ROOT)), 'producer_sha256': package_build.sha(__file__),
                      'compiler_environment_sha256': package_build.sha(package_build.__file__),
                      'config_sha256': package_build.sha(CONFIG), 'recipe': 'original Debian debian/rules and control',
                      'build_command': build_command, 'build_profiles': PROFILES,
                      'build_packages': debian.build_packages(root),
                      'runtime_dependencies': debian.runtime_dependencies(output, packages)}
            verify_debian_packages(record, data=data)
    debian.write_manifest(output, record)
    incomplete.unlink()
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/libcamera')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    print(json.dumps(build(args.output, args.sysroot, args.jobs), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
