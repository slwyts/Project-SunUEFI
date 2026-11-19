"""Build the Arch Linux ARM libcamera split packages from the native recipe."""
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

import build_arch_package
from build_piano_ram_bootstrap import guest_resolve
from build import overlay_packages, overlay_release
from desktop import build_libcamera_packages, package_build
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[2]
RECIPE_PATCH = ROOT / 'linux/desktops/common/libcamera/PKGBUILD.patch'
PACKAGES = frozenset(('libcamera', 'libcamera-docs', 'libcamera-ipa', 'libcamera-tools',
                      'gst-plugin-libcamera', 'python-libcamera'))
RUNTIME_INPUTS = ('libcamera', 'libcamera-ipa')
PYTHON_FIX = 'libcamera-fix-python3.14-macro-redefinition.patch'


def prepare(recipe, data, patches, version, jobs):
    source = data['source']
    arch = data['arch']
    cache = ROOT / 'build/source-cache/libcamera'
    cache.mkdir(parents=True, exist_ok=True)
    source_dir = 'libcamera-' + source['commit']
    source_name = source_dir + '.tar.gz'
    archive = cache / (source['sha256'] + '.tar.gz')
    recipe.mkdir(parents=True, exist_ok=True)
    with (cache / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        download(source['url'], archive, source['sha256'])
        inputs = {}
        for name, digest in arch['files'].items():
            path = cache / (digest + '-' + name)
            download(arch['recipe_url'] + '/' + name, path, digest)
            inputs[name] = path
    shutil.copy2(archive, recipe / source_name)
    for path in patches:
        shutil.copy2(path, recipe / path.name)

    # The pinned upstream tarball adds one compiler flag before this hunk.
    python_patch = inputs[PYTHON_FIX].read_text()
    context = "@@ -55,6 +55,7 @@ pycamera_args = [\n     '-fvisibility=hidden',"
    if python_patch.count(context) != 1:
        raise ValueError('Rebase the Arch Python binding patch onto the pinned libcamera source')
    (recipe / PYTHON_FIX).write_text(python_patch.replace(
        context, "@@ -56,6 +56,7 @@ pycamera_args = [\n     '-Wno-invalid-offsetof',"))

    text = subprocess.check_output(
        ['patch', '--silent', '--batch', '--fuzz=0', '-o', '-', inputs['PKGBUILD'], '-i', RECIPE_PATCH],
        text=True)
    patch_names = [path.name for path in patches] + [PYTHON_FIX]
    sources = [source_name, *patch_names]
    hashes = [package_build.sha(recipe / name) for name in sources]
    values = {'VERSION': version, 'RELEASE': arch['package_release'],
              'SOURCE_DIR': source_dir, 'JOBS': str(jobs),
              'PATCHES': shlex.join(patch_names), 'SOURCES': shlex.join(sources),
              'CHECKSUMS': shlex.join(hashes)}
    for key, value in values.items():
        text = text.replace('@' + key + '@', value)
    (recipe / 'PKGBUILD').write_text(text)


def sonames(abi):
    return {f'libcamera.so={abi}-64', f'libcamera-base.so={abi}-64'}


def verify_dependencies(fields, name, version, abi):
    if name == 'libcamera':
        if not sonames(abi) <= set(fields.get('provides', ())):
            raise ValueError('libcamera must provide the package ABI')
        if f'libcamera-ipa={version}' not in fields.get('depend', ()):
            raise ValueError('libcamera must install with the IPA from the same build')
    elif name != 'libcamera-docs':
        if not {f'libcamera={version}', *sonames(abi)} <= set(fields.get('depend', ())):
            raise ValueError('The package does not require its matching libcamera ABI: ' + name)


def verify_arch_packages(runtime, archives, version, abi):
    packages = {}
    for archive, row in archives.items():
        fields = overlay_packages.pacman_info(runtime / archive)
        name, = fields['pkgname']
        if (name in packages or name not in PACKAGES or
                fields['arch'] != ['aarch64'] or fields['pkgver'] != [version]):
            raise ValueError('Unexpected native libcamera package: ' + archive)
        verify_dependencies(fields, name, version, abi)
        packages[name] = {'file': archive, 'sha256': row['sha256'],
                          'version': version, 'architecture': 'aarch64'}
    if set(packages) != PACKAGES:
        raise ValueError('The build must produce the complete Arch libcamera split package set')
    return packages


def runtime_inputs(directory):
    """Read matching library and IPA archives from a finished native build."""
    directory = Path(directory).resolve()
    runtime = directory / 'runtime'
    data = build_libcamera_packages.policy()
    abi = build_libcamera_packages.abi_version(data)
    upstream = build_libcamera_packages.upstream_version(data)
    record = json.loads((directory / 'SOURCE.json').read_text())
    manifest = json.loads((runtime / 'manifest.json').read_text())
    version = record.get('package_version', '')
    if (record.get('packages_built') is not True or (directory / '.incomplete').exists() or
            record.get('source') != data['source'] or record.get('package_format') != 'pacman' or
            manifest.get('package_format') != 'pacman' or manifest.get('architecture') != 'aarch64' or
            record.get('recipe_sha256') != manifest.get('recipe_sha256') or
            not manifest.get('recipe_sha256') or
            not re.fullmatch(re.escape(upstream) + r'-[1-9][0-9]*(?:\.[0-9]+)?', version)):
        raise ValueError('Use a completed Arch libcamera build from the selected sources')
    inputs = {}
    for name in RUNTIME_INPUTS:
        row = record.get('packages', {}).get(name, {})
        filename = row.get('file', '')
        checksum = row.get('sha256')
        base = re.escape(f'{name}-{version}-aarch64')
        if not re.fullmatch(base + overlay_packages.PACMAN_SUFFIX, filename):
            raise ValueError('Missing matching Arch libcamera archive: ' + name)
        archive = runtime / filename
        if (archive.is_symlink() or not archive.is_file() or
                (row.get('version'), row.get('architecture')) != (version, 'aarch64') or
                manifest['packages'].get(filename, {}).get('sha256') != checksum or
                package_build.sha(archive) != checksum):
            raise ValueError('Arch libcamera archive differs from the build record: ' + filename)
        fields = overlay_packages.pacman_info(archive)
        if (fields.get('pkgname'), fields.get('pkgver'), fields.get('arch')) != (
                [name], [version], ['aarch64']):
            raise ValueError('Unexpected Arch libcamera package: ' + filename)
        verify_dependencies(fields, name, version, abi)
        inputs[name] = row
    return runtime, version, abi, inputs


def build(root, output, jobs, *, data, patches, proof, version, abi):
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or jobs < 1 or not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or root.is_relative_to(output) or
            output.is_relative_to(root)):
        raise ValueError('Use root, an isolated Arch build root, a separate output, and at least one job')
    release = guest_resolve(root, '/etc/os-release').read_text().splitlines()
    distro = next((line[3:].strip('"\'') for line in release if line.startswith('ID=')), '')
    if distro not in ('archarm', 'archlinuxarm'):
        raise ValueError('This libcamera recipe is for Arch Linux ARM')
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock, (output / 'build.log').open('a') as log:
        fcntl.flock(lock, fcntl.LOCK_EX)
        (output / 'SOURCE.json').unlink(missing_ok=True)
        recipe = output / 'recipe'
        prepare(recipe, data, patches, version, jobs)
        runtime = output / 'runtime'
        work = root / 'var/tmp/sunuefi-package-build'
        try:
            subprocess.run(
                [sys.executable, build_arch_package.__file__, '--rootfs', root, '--recipe', recipe,
                 '--output', runtime, '--jobs', str(jobs),
                 *(['--resume'] if work.is_dir() else [])],
                check=True, stdout=log, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            raise ValueError(f'Arch libcamera packaging failed; see {log.name}') from exc
        manifest = json.loads((runtime / 'manifest.json').read_text())
        pkgver = version + '-' + overlay_release.pacman_release(data['arch']['package_release'])
        packages = verify_arch_packages(runtime, manifest['packages'], pkgver, abi)
        record = {'packages_built': True, 'package_version': pkgver, 'package_format': 'pacman',
                  'packages': packages, 'source': data['source'], 'arch_recipe': data['arch'],
                  'patches': proof, 'recipe_sha256': manifest['recipe_sha256']}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        return record
