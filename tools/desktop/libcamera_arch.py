"""Build native libcamera split packages for Arch Linux ARM and SteamOS."""
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
from package.archive import pacman_info
from desktop import build_libcamera_packages, package_build
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[2]
RECIPE_PATCH = ROOT / 'linux/desktops/common/libcamera/PKGBUILD.patch'
STEAMOS_PACKAGES = frozenset(('libcamera', 'libcamera-ipa', 'libcamera-tools',
                              'gst-plugin-libcamera', 'python-libcamera'))
PACKAGES = STEAMOS_PACKAGES | {'libcamera-docs'}
RUNTIME_INPUTS = ('libcamera', 'libcamera-ipa')
PYTHON_FIX = 'libcamera-fix-python3.14-macro-redefinition.patch'
VALVE_DB_PATH = '/usr/lib/holo/pacmandb'


def build_target(root):
    release = {key: value.strip('"\'') for line in
               guest_resolve(root, '/etc/os-release').read_text().splitlines()
               for key, sep, value in (line.partition('='),) if sep}
    distro = release.get('ID')
    if distro == 'steamos':
        return 'steamos'
    if (distro in ('arch', 'archarm', 'archlinuxarm') and
            guest_resolve(root, VALVE_DB_PATH + '/local').is_dir()):
        # Valve's filesystem update can replace os-release with Arch's file
        # while the system still uses Valve's pacman database and libraries.
        database = subprocess.check_output(
            ['chroot', root, 'pacman-conf', 'DBPath'], text=True).strip()
        if database.rstrip('/') == VALVE_DB_PATH:
            return 'steamos'
    if distro in ('archarm', 'archlinuxarm'):
        return 'arch'
    raise ValueError('Use an Arch Linux ARM or SteamOS build root')


def recipe_policy(data, target):
    if target == 'arch':
        return data['arch']
    if target == 'steamos':
        return {**data['arch'], **data['steamos']}
    raise ValueError('Unknown pacman libcamera build target: ' + target)


def prepare(recipe, data, patches, version, jobs, *, target='arch'):
    source = data['source']
    settings = recipe_policy(data, target)
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
        for name, digest in settings['files'].items():
            path = cache / (digest + '-' + name)
            download(settings['recipe_url'] + '/' + name, path, digest)
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
    steamos = target == 'steamos'
    values = {'VERSION': version, 'RELEASE': settings['package_release'],
              'SOURCE_DIR': source_dir, 'JOBS': str(jobs),
              'PATCHES': shlex.join(patch_names), 'SOURCES': shlex.join(sources),
              'CHECKSUMS': shlex.join(hashes),
              'STEAMOS': 'true' if steamos else 'false',
              'PIPELINES': ','.join(settings.get('pipelines', ())),
              'GLIB_FIX': settings.get('glib_parent_class_fix', '')}
    for key, value in values.items():
        text = text.replace('@' + key + '@', value)
    (recipe / 'PKGBUILD').write_text(text)


def sonames(abi):
    return {f'libcamera.so={abi}-64', f'libcamera-base.so={abi}-64'}


def verify_dependencies(fields, name, version, abi, *, target='arch'):
    depends = set(fields.get('depend', ()))
    if name == 'libcamera':
        if not sonames(abi) <= set(fields.get('provides', ())):
            raise ValueError('libcamera must provide the package ABI')
        if f'libcamera-ipa={version}' not in depends:
            raise ValueError('libcamera must install with the IPA from the same build')
        if target == 'steamos' and any(item.startswith('libpisp') for item in depends):
            raise ValueError('SteamOS libcamera must not require the ALARM libpisp package')
    elif name == 'libcamera-docs':
        if any(fields.get(key) for key in ('depend', 'optdepend', 'provides', 'conflict', 'replaces')):
            raise ValueError('libcamera-docs must be installable without switching any runtime packages')
    else:
        if not {f'libcamera={version}', *sonames(abi)} <= depends:
            raise ValueError('The package does not require its matching libcamera ABI: ' + name)
        if (target == 'steamos' and name == 'libcamera-tools' and
                not any(re.fullmatch(r'qt6-base>=6\.[0-9]+', item) for item in depends)):
            raise ValueError('SteamOS qcam must require the Qt minor version used to build it')


def verify_arch_packages(runtime, archives, version, abi, *, target='arch'):
    expected = STEAMOS_PACKAGES if target == 'steamos' else PACKAGES
    packages = {}
    for archive, row in archives.items():
        path = runtime / archive
        if Path(archive).name != archive or path.is_symlink() or package_build.sha(path) != row['sha256']:
            raise ValueError('Native libcamera archive differs from the build record: ' + archive)
        fields = pacman_info(path)
        name, = fields['pkgname']
        if (name in packages or name not in expected or
                fields['arch'] != ['aarch64'] or fields['pkgver'] != [version]):
            raise ValueError('Unexpected native libcamera package: ' + archive)
        verify_dependencies(fields, name, version, abi, target=target)
        packages[name] = {'file': archive, 'sha256': row['sha256'],
                          'version': version, 'architecture': 'aarch64'}
    if set(packages) != expected:
        raise ValueError('The build must produce the complete Arch libcamera split package set')
    return packages


def runtime_inputs(directory, *, target='arch'):
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
            record.get('arch_recipe') != recipe_policy(data, target) or
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
        fields = pacman_info(archive)
        if (fields.get('pkgname'), fields.get('pkgver'), fields.get('arch')) != (
                [name], [version], ['aarch64']):
            raise ValueError('Unexpected Arch libcamera package: ' + filename)
        verify_dependencies(fields, name, version, abi, target=target)
        inputs[name] = row
    return runtime, version, abi, inputs


def build(root, output, jobs, *, data, patches, proof, version, abi):
    root = root.resolve()
    if os.geteuid() or jobs < 1 or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root, an isolated Arch build root, a separate output, and at least one job')
    target = build_target(root)
    output = (output or ROOT / 'build' / f'libcamera-{target}').resolve()
    if not output.is_relative_to(ROOT / 'build') or root.is_relative_to(output) or output.is_relative_to(root):
        raise ValueError('Use a separate libcamera package output under build/')
    settings = recipe_policy(data, target)
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock, (output / 'build.log').open('a') as log:
        fcntl.flock(lock, fcntl.LOCK_EX)
        (output / 'SOURCE.json').unlink(missing_ok=True)
        recipe = output / 'recipe'
        prepare(recipe, data, patches, version, jobs, target=target)
        runtime = output / 'runtime'
        work = root / 'var/tmp/sunuefi-package-build'
        try:
            subprocess.run(
                [sys.executable, build_arch_package.__file__, '--rootfs', root, '--recipe', recipe,
                 '--output', runtime, '--jobs', str(jobs), '--build-only',
                 *(['--resume'] if work.is_dir() else [])],
                check=True, stdout=log, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            raise ValueError(f'Arch libcamera packaging failed; see {log.name}') from exc
        manifest = json.loads((runtime / 'manifest.json').read_text())
        pkgver = version + '-' + overlay_release.pacman_release(settings['package_release'])
        packages = verify_arch_packages(runtime, manifest['packages'], pkgver, abi, target=target)
        installed = set(subprocess.check_output(['chroot', root, 'pacman', '-Qq'], text=True).splitlines())
        selected = overlay_packages.PACMAN_CAMERA_DEFAULTS['libcamera'] | (packages.keys() & installed)
        build_arch_package.install_cached(
            root, runtime,
            selected_archives={packages[name]['file']: {'sha256': packages[name]['sha256']}
                               for name in sorted(selected)})
        record = {'packages_built': True, 'package_version': pkgver, 'package_format': 'pacman',
                  'packages': packages, 'source': data['source'], 'target': target, 'arch_recipe': settings,
                  'patches': proof, 'recipe_sha256': manifest['recipe_sha256']}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        return record
