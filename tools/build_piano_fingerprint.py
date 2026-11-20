#!/usr/bin/env python3
"""Build the native Goodix client with the standard Linux TEE object uAPI."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import shlex
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / 'linux/userspace/fingerprint'
ARCH_LIBFPRINT_RECIPE = ROOT / 'linux/bsp/arch/libfprint/PKGBUILD.in'
STEAMOS_FPRINTD_RECIPE = ROOT / 'linux/bsp/steamos/fprintd/PKGBUILD.in'
CORE_SOURCES = (
    'piano-goodix-storage.c', 'piano-goodix-events.c',
    'piano-goodix-gatekeeper.c', 'piano-goodix-operations.c',
    'piano-goodix-rpmb.c', 'piano-goodix-tee.c', 'piano-goodix.c',
)
HEADERS = (
    'piano-goodix-storage.h', 'piano-goodix-events.h',
    'piano-goodix-gatekeeper.h', 'piano-goodix-operations.h',
    'piano-goodix-rpmb.h', 'piano-goodix-tee.h', 'piano-goodix-storage-protocol.h', 'piano-goodix.h',
)
CLI_SOURCE = NATIVE / 'piano-goodix-cli.c'
LICENSES = ('BSD-3-Clause.txt', 'BSD-3-Clause-Clear.txt')
PACMAN_LIBFPRINT_RUNTIME = ('libgcc', 'glib2', 'glibc', 'libgudev', 'libgusb', 'openssl', 'pixman')
STEAMOS_FPRINTD_RUNTIME = ('dbus', 'glib2', 'glibc', 'libgcc', 'libfprint',
                         'pam', 'polkit', 'systemd', 'systemd-libs')
LIBFPRINT_SONAME = 'libfprint-2.so=2-64'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patches(record):
    if 'patch_series' not in record:
        return []
    series = ROOT / record['patch_series']
    return [(series.parent / entry['file'], entry['sha256'])
            for entry in json.loads(series.read_text())['patches']]


def source_inputs():
    config = json.loads((ROOT / 'config/fingerprint.json').read_text())
    patch_inputs = {}
    for record in (*config['sources'].values(), *config['desktop'].values()):
        if 'patch_series' in record:
            series = ROOT / record['patch_series']
            patch_inputs[record['patch_series']] = digest(series)
        for path, expected in patches(record):
            if digest(path) != expected:
                raise ValueError('Fingerprint patch checksum differs: ' + str(path))
            patch_inputs[str(path.relative_to(ROOT))] = expected
    return {
        'config_sha256': digest(ROOT / 'config/fingerprint.json'),
        'native_sources': {name: digest(NATIVE / name)
                           for name in (*CORE_SOURCES, *HEADERS, *LICENSES)},
        'builder_sha256': digest(Path(__file__)),
        'kernel_uapi_sha256': json.loads(
            (ROOT / 'config/release.json').read_text())['kernel']['uapi_sha256'],
        'firmware_manifest_sha256': digest(
            ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json'),
        'patches': patch_inputs,
    }


def source(name, record):
    path = ROOT / 'build/source-cache' / name
    fresh = not path.exists()
    if fresh:
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout',
                        record['url'], path], check=True)
    if not fresh and subprocess.check_output(
            ['git', '-C', path, 'status', '--porcelain', '--untracked-files=no'],
            text=True):
        raise ValueError('Fingerprint dependency has local changes: ' + name)
    if subprocess.run(['git', '-C', path, 'cat-file', '-e', record['commit']],
                      capture_output=True).returncode:
        subprocess.run(['git', '-C', path, 'fetch', '--depth=1', 'origin',
                        record['commit']], check=True)
    subprocess.run(['git', '-C', path, 'checkout', '--detach', record['commit']],
                   check=True)
    return path


def stage_sources(work, uapi):
    """Keep one source-built backend for the CLI and native libfprint builds."""
    work.mkdir(parents=True)
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['sources']
    dependencies = {}
    for name, record in cfg.items():
        upstream = source(name, record)
        destination, archive = work / name, work / (name + '.tar')
        destination.mkdir()
        subprocess.run(['git', '-C', upstream, 'archive', '--format=tar',
                        '--output=' + str(archive.resolve()), record['commit']], check=True)
        subprocess.run(['tar', '-xf', archive, '-C', destination], check=True)
        archive.unlink()
        for path, expected in patches(record):
            if digest(path) != expected:
                raise ValueError('Fingerprint patch checksum differs: ' + str(path))
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path],
                           cwd=destination, check=True)
        dependencies[name] = destination
    for name in (*CORE_SOURCES, *HEADERS, *LICENSES):
        shutil.copy2(NATIVE / name, work / name)
    headers = work / 'uapi/linux'
    headers.mkdir(parents=True)
    shutil.copy2(uapi / 'include/linux/tee.h', headers / 'tee.h')
    return dependencies


def backend_archive(output, backend):
    archive = backend.parent / 'piano-goodix-sources.tar'
    subprocess.run(['tar', '--sort=name', '--mtime=@0', '--owner=0', '--group=0',
                    '--numeric-owner', '-cf', archive, '-C', backend, '.'], check=True)
    return {'file': str(archive.relative_to(output)), 'sha256': digest(archive)}


def prepare_desktop_backend(output, uapi):
    """Export the reviewed source used by libfprint without building the CLI."""
    inputs = source_inputs()
    tee_header = uapi / 'include/linux/tee.h'
    tee_digest = digest(tee_header)
    backend = output / 'fingerprint/backend'
    stage_sources(backend, uapi)
    archive = backend_archive(output, backend)
    if source_inputs() != inputs:
        raise ValueError('Fingerprint inputs changed while preparing the backend')
    if digest(tee_header) != tee_digest or digest(backend / 'uapi/linux/tee.h') != tee_digest:
        raise ValueError('Fingerprint TEE header changed while preparing the backend')
    record = {'inputs': inputs, 'backend_archive': archive,
              'uapi_headers': {'linux/tee.h': tee_digest}}
    (backend.parent / 'fingerprint.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def backend_sources(runtime):
    record = json.loads((runtime / 'fingerprint/fingerprint.json').read_text())
    if record.get('inputs') != source_inputs():
        raise ValueError('Rebuild the fingerprint runtime sources before packaging libfprint')
    archive = record.get('backend_archive', {})
    if archive.get('file') != 'fingerprint/piano-goodix-sources.tar':
        raise ValueError('Rebuild the fingerprint runtime; its backend source archive is missing')
    path = runtime / archive['file']
    if path.is_symlink() or not path.is_file() or digest(path) != archive.get('sha256'):
        raise ValueError('Fingerprint backend source archive differs from its build record')
    return path


def pacman_desktop_recipes(output, runtime, *, steamos):
    """Build libfprint with Goodix support and, on SteamOS, upstream fprintd."""
    config = json.loads((ROOT / 'config/fingerprint.json').read_text())
    cfg = config['desktop']
    required = {'libfprint': PACMAN_LIBFPRINT_RUNTIME}
    crypto = f'libcrypto.so={config["steamos"]["openssl_abi"]}-64'
    if steamos:
        required['fprintd'] = STEAMOS_FPRINTD_RUNTIME
    backend = backend_sources(runtime)
    output.mkdir(parents=True, exist_ok=True)
    for name in required:
        record = cfg[name]
        recipe = output / name
        recipe.mkdir(exist_ok=True)
        upstream = source(name, record)
        archive = recipe / (name + '.tar')
        subprocess.run(['git', '-C', upstream, 'archive', '--format=tar',
                        '--prefix=' + name + '/', '--output=' + str(archive.resolve()),
                        record['commit']], check=True)
        patch_names = []
        for path, expected in patches(record):
            if digest(path) != expected:
                raise ValueError('Fingerprint desktop patch changed: ' + str(path))
            shutil.copy2(path, recipe / path.name)
            patch_names.append(path.name)
        local = ['piano-goodix-sources.tar'] if name == 'libfprint' else []
        for filename in local:
            shutil.copy2(backend, recipe / filename)
        files = [archive.name, *patch_names, *local]
        hashes = [digest(recipe / file) for file in files]
        values = {'VERSION': record['version'], 'SOURCES': shlex.join(files),
                  'CHECKSUMS': shlex.join(hashes)}
        if name == 'libfprint':
            template = ARCH_LIBFPRINT_RECIPE
            depends = (*required[name], crypto) if steamos else required[name]
            values.update(RELEASE=config['arch']['package_release'], PATCHES=shlex.join(patch_names),
                          OPTIONS='options=(!debug !lto)\n' if steamos else '')
        else:
            template = STEAMOS_FPRINTD_RECIPE
            depends = (*required[name], LIBFPRINT_SONAME)
            values['RELEASE'] = config['steamos']['fprintd_package_release']
        values['DEPENDENCIES'] = shlex.join(depends)
        text = template.read_text()
        for key, value in values.items():
            text = text.replace('@' + key + '@', value)
        (recipe / 'PKGBUILD').write_text(text)
    return required


def install_pacman_desktop(root, runtime, output, *, steamos):
    import build_arch_package
    from build_nyarch_apps import guest_environment
    from build_release_rootfs import build_resolver

    root, runtime, output = root.resolve(), runtime.resolve(), output.resolve()
    if (not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root)):
        raise ValueError('Build fingerprint packages outside a project-built target rootfs')
    recipes = output / 'recipes'
    dependencies = pacman_desktop_recipes(recipes, runtime, steamos=steamos)
    for name, required in dependencies.items():
        missing = subprocess.run(['chroot', root, 'pacman', '-T', *required],
                                 capture_output=True, text=True)
        if missing.returncode not in (0, 127):
            raise RuntimeError(missing.stderr)
        if missing.returncode:
            with build_resolver(root), guest_environment(root) as guest:
                guest(['pacman', '-S', '--needed', '--noconfirm', *missing.stdout.splitlines()])
        packages = output / name
        cohort = subprocess.check_output(
            ['chroot', root, 'pacman', '-Q', *required], text=True)
        inputs = {'recipe_sha256': build_arch_package.recipe_sha256(recipes / name / 'PKGBUILD'),
                  'runtime_packages': cohort}
        if not (packages / 'manifest.json').is_file():
            build_arch_package.build(root, recipes / name, packages)
            updated = subprocess.check_output(
                ['chroot', root, 'pacman', '-Q', *required], text=True)
            if updated != cohort:
                raise ValueError('Fingerprint build dependencies changed the target ABI; rebuild the packages')
            (packages / 'fingerprint-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        else:
            marker = packages / 'fingerprint-inputs.json'
            if not marker.is_file() or json.loads(marker.read_text()) != inputs:
                raise ValueError('Fingerprint package sources or target ABI changed; use a fresh output')
            build_arch_package.install_cached(root, packages)
    with build_resolver(root), guest_environment(root) as guest:
        guest(['pacman', '-T', LIBFPRINT_SONAME])
        if not steamos:
            guest(['pacman', '-S', '--needed', '--noconfirm', 'extra/fprintd'])
    return dependencies.keys()
