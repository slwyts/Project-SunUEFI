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
RUNTIME_PATHS = {
    'usr/local/sbin/piano-fingerprint-ctl',
    'usr/share/piano-provenance/fingerprint.json',
    'usr/share/licenses/piano-fingerprint/linux-tee.h',
    *{f'usr/share/licenses/piano-fingerprint/{name}.txt'
       for name in ('qcbor',)},
    *{f'usr/share/licenses/piano-fingerprint/{name}' for name in LICENSES},
}


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
                           for name in (*CORE_SOURCES, *HEADERS, *LICENSES, CLI_SOURCE.name)},
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
    backend = output / 'fingerprint/backend'
    stage_sources(backend, uapi)
    archive = backend_archive(output, backend)
    if source_inputs() != inputs:
        raise ValueError('Fingerprint inputs changed while preparing the backend')
    (backend.parent / 'fingerprint.json').write_text(json.dumps(
        {'inputs': inputs, 'backend_archive': archive}, indent=2) + '\n')


def build(output, flags, uapi, emulator=None):
    inputs = source_inputs()
    tee_header = uapi / 'include/linux/tee.h'
    tee_digest = digest(tee_header)
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())
    work = output / 'fingerprint'
    work.mkdir()
    backend = work / 'backend'
    dependencies = stage_sources(backend, uapi)
    # Use the exported kernel interface before any sysroot system headers.
    flags = [flags[0], '-isystem', str(backend / 'uapi'), *flags[1:]]
    qcbor = dependencies['qcbor']
    includes = [qcbor / 'inc']
    originals = [
        *[qcbor / 'src' / name for name in (
            'UsefulBuf.c', 'qcbor_encode.c', 'qcbor_decode.c',
            'qcbor_err_to_str.c', 'ieee754.c')],
        *[backend / name for name in CORE_SOURCES],
        CLI_SOURCE,
    ]
    native_sources = {backend / name for name in CORE_SOURCES} | {CLI_SOURCE}
    objects = []
    for index, original in enumerate(originals):
        obj = work / (str(index) + '.o')
        command = [*flags, '-fPIC', '-D_GNU_SOURCE', '-DUSE_QCBOR',
                   *['-I' + str(p) for p in includes],
                   '-c', original, '-o', obj]
        # Imported Qualcomm sources retain their own warning policy.
        if original not in native_sources:
            command = [arg for arg in command if arg != '-Werror']
        subprocess.run(command, check=True)
        objects.append(obj)
    archive = backend_archive(output, backend)
    binary = work / 'piano-fingerprint-ctl'
    subprocess.run([*flags, '-static', *objects, '-pthread', '-lm', '-o', binary], check=True)
    subprocess.run(([emulator] if emulator else []) + [str(binary), '--help'], check=True)
    files = {}
    for name, path, mode in [
            ('usr/local/sbin/piano-fingerprint-ctl', binary, 0o755),
            ('usr/share/licenses/piano-fingerprint/linux-tee.h',
             backend / 'uapi/linux/tee.h', 0o644),
            *[(f'usr/share/licenses/piano-fingerprint/{name}', NATIVE / name, 0o644)
              for name in LICENSES],
            *[(f'usr/share/licenses/piano-fingerprint/{name}.txt',
               dependencies[name] / record['license'], 0o644)
              for name, record in cfg['sources'].items()]]:
        target = work / Path(name).name
        if path != target:
            shutil.copy2(path, target)
        files[name] = {'file': str(target.relative_to(output)),
                       'bytes': target.stat().st_size, 'sha256': digest(target), 'mode': mode}
    record = {
        'inputs': inputs,
        'sources': cfg['sources'],
        'client_source': str(CLI_SOURCE.relative_to(ROOT)),
        'client_sha256': digest(CLI_SOURCE),
        'source_files': {str(p.relative_to(ROOT)): digest(p) for p in originals},
        'backend_archive': archive,
        'uapi_headers': {'linux/tee.h': tee_digest},
        'firmware': json.loads(
            (ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json').read_text())['files'],
        'operations': ['info', 'initialize', 'prepare-auth', 'watch', 'list',
                       'storage-probe', 'enroll', 'verify', 'delete'],
    }
    if source_inputs() != inputs:
        raise ValueError('Fingerprint sources changed during compilation')
    if digest(tee_header) != tee_digest or digest(backend / 'uapi/linux/tee.h') != tee_digest:
        raise ValueError('Fingerprint TEE header changed during compilation')
    provenance = work / 'fingerprint.json'
    provenance.write_text(json.dumps(record, indent=2) + '\n')
    files['usr/share/piano-provenance/fingerprint.json'] = {
        'file': str(provenance.relative_to(output)), 'bytes': provenance.stat().st_size,
        'sha256': digest(provenance), 'mode': 0o644}
    return files, record


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


DEBIAN_ABI_PACKAGES = (
    'libc6', 'libglib2.0-0t64', 'libgusb2a', 'libgudev-1.0-0', 'libpixman-1-0', 'libssl3t64',
)


def debian_abi(root):
    packages = subprocess.check_output(
        ['dpkg-query', '--admindir=' + str(root / 'var/lib/dpkg'), '-W',
         '-f=${Package}\t${Version}\t${Architecture}\t${db:Status-Status}\n',
         *DEBIAN_ABI_PACKAGES], text=True)
    versions = {}
    for line in packages.splitlines():
        name, version, architecture, status = line.split('\t')
        if (name not in DEBIAN_ABI_PACKAGES or architecture != 'arm64' or
                status != 'installed' or not version):
            raise ValueError('Unexpected fingerprint target ABI package: ' + line)
        versions[name] = version
    if set(versions) != set(DEBIAN_ABI_PACKAGES):
        raise ValueError('Fingerprint target ABI package set is incomplete')
    return {'os_release_sha256': digest(root / 'etc/os-release'), 'packages': versions}


def shlib_dependencies(output):
    rows = [line.removeprefix('shlibs:Depends=') for line in output.splitlines()
            if line.startswith('shlibs:Depends=')]
    if len(rows) != 1 or not rows[0]:
        raise ValueError('dpkg-shlibdeps did not produce fingerprint runtime dependencies')
    return rows[0]


def debian_package_fields(path):
    text = subprocess.check_output(
        ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture', 'Depends'], text=True)
    return dict(line.split(': ', 1) for line in text.splitlines())


def debian_reusable(output, inputs, target_abi, backend_archive):
    """Reuse only the exact source generation and target runtime package ABI."""
    from build import overlay_release

    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']['libfprint']
        package = record['package']
        filename = package['file']
        version = overlay_release.debian_version('1:' + cfg['version'] + '+sunuefi2')
        if (record['inputs'] != inputs or record['target_abi'] != target_abi or
                record['backend_sources_sha256'] != digest(backend_archive) or
                package['version'] != version or package['architecture'] != 'arm64' or
                Path(filename).name != filename or not filename.endswith('.deb') or
                not record['dependencies']):
            return None
        # Existing complete outputs kept the deb beside SOURCE.json. New
        # outputs use runtime/, as other desktop package builders do.
        folder = output / 'runtime' if (output / 'runtime').is_dir() else output
        path = folder / filename
        if path.is_symlink() or digest(path) != package['sha256']:
            return None
        if debian_package_fields(path) != {
                'Package': 'libfprint-2-2', 'Version': version, 'Architecture': 'arm64',
                'Depends': record['dependencies']}:
            return None
        return record, path
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError):
        return None


def install_debian_package(root, path, target_abi):
    """Check the target loader and APT dependencies before replacing libfprint."""
    from build_release_rootfs import build_devices

    if debian_abi(root) != target_abi:
        raise ValueError('Fingerprint target ABI changed before package installation')
    with tempfile.TemporaryDirectory(prefix='sunuefi-libfprint-install-', dir=root / 'var/tmp') as folder:
        install = Path(folder)
        package = install / 'package'
        subprocess.run(['dpkg-deb', '-x', path, package], check=True)
        shutil.copy2(path, install / path.name)
        library = package / 'usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0'
        elf = library.read_bytes()
        if elf[:6] != b'\x7fELF\x02\x01' or elf[18:20] != b'\xb7\x00':
            raise ValueError('Cached libfprint must be an ARM64 ELF')
        library_path = '/' + library.parent.relative_to(root).as_posix()
        daemon = next((name for name in ('/usr/libexec/fprintd', '/usr/lib/fprintd/fprintd')
                       if (root / name.lstrip('/')).is_file()), None)
        if daemon is None:
            raise ValueError('The target fprintd executable is missing')
        with build_devices(root):
            for binary in (library_path + '/' + library.name, daemon):
                result = subprocess.run(['chroot', root, 'env', 'LC_ALL=C',
                                         'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                                         'LD_LIBRARY_PATH=' + library_path, 'ldd', '-r', binary],
                                        capture_output=True, text=True, check=True)
                messages = result.stdout + result.stderr
                if ('not found' in messages or 'undefined symbol' in messages or
                        (binary == daemon and
                         'libfprint-2.so.2 => ' + library_path + '/libfprint-2.so.2' not in messages)):
                    raise ValueError('Fingerprint library or fprintd does not resolve against the target ABI')
            guest_deb = '/' + (install / path.name).relative_to(root).as_posix()
            subprocess.run(['chroot', root, 'env', 'LC_ALL=C',
                            'PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'apt-get', '--simulate',
                            '--no-remove', '--no-install-recommends', 'install', guest_deb], check=True)
            subprocess.run(['chroot', root, 'env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                            'dpkg', '-i', guest_deb], check=True)


def install_debian_desktop(root, runtime, output):
    from build import overlay_release
    from build_release_rootfs import build_devices, build_resolver
    from desktop import debian_package, package_build
    root, runtime, output = root.resolve(), runtime.resolve(), output.resolve()
    if (not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root)):
        raise ValueError('Build fingerprint packages outside a project-built target rootfs')
    output.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']['libfprint']
    backend_archive = backend_sources(runtime)
    inputs = source_inputs()
    architecture = subprocess.check_output(
        ['chroot', root, 'dpkg', '--print-architecture'], text=True).strip()
    if architecture != 'arm64':
        raise ValueError('The fingerprint target must use ARM64 Debian packages')
    # Runtime packages belong in the image; compilers and -dev packages do not.
    with build_devices(root), build_resolver(root):
        subprocess.run(['chroot', root, 'env', 'DEBIAN_FRONTEND=noninteractive',
                        'PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'apt-get', 'install', '-y',
                        '--no-upgrade', '--no-remove', '--no-install-recommends',
                        'fprintd', 'libpam-fprintd'], check=True)
    target_abi = debian_abi(root)
    cached = debian_reusable(output, inputs, target_abi, backend_archive)
    if cached:
        print('Reusing completed fingerprint package: ' + str(cached[1]), flush=True)
        install_debian_package(root, cached[1], target_abi)
        return
    identity = hashlib.sha256(json.dumps({
        'inputs': inputs, 'target_abi': target_abi,
        'backend_sources_sha256': digest(backend_archive),
    }, sort_keys=True).encode()).hexdigest()[:16]
    builder = output / 'builder' / identity / 'rootfs'
    if not builder.exists():
        builder.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-a', '--reflink=auto', root, builder], check=True)
    elif debian_abi(builder) != target_abi:
        raise ValueError('Existing fingerprint builder differs from the target ABI')
    debian_package.verify_build_archive(builder)
    work = builder / 'var/tmp/sunuefi-libfprint'
    if work.exists():
        raise ValueError('Inspect the interrupted fingerprint build before retrying: ' + str(work))
    work.mkdir(parents=True)
    upstream = source('libfprint', cfg)
    archive = output / 'libfprint.tar'
    subprocess.run(['git', '-C', upstream, 'archive', '--prefix=libfprint/',
                    '--output=' + str(archive.resolve()), cfg['commit']], check=True)
    subprocess.run(['tar', '-xf', archive, '-C', work], check=True)
    for path, expected in patches(cfg):
        if digest(path) != expected:
            raise ValueError('libfprint patch checksum changed')
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', work / 'libfprint',
                        '-i', path], check=True)
    backend = work / 'libfprint/libfprint/drivers/piano-goodix'
    backend.mkdir()
    subprocess.run(['tar', '-xf', backend_archive, '-C', backend], check=True)
    with build_devices(builder), build_resolver(builder), package_build.environment(builder) as settings:
        def guest(args, **kwargs):
            return subprocess.run(['chroot', builder, 'env', 'DEBIAN_FRONTEND=noninteractive',
                                   *package_build.arguments(settings), *args], check=True, **kwargs)

        versions = target_abi['packages']
        development = {
            'libc6-dev': 'libc6', 'libglib2.0-dev': 'libglib2.0-0t64',
            'libgusb-dev': 'libgusb2a', 'libgudev-1.0-dev': 'libgudev-1.0-0',
            'libpixman-1-dev': 'libpixman-1-0', 'libssl-dev': 'libssl3t64',
        }
        guest(['apt-get', 'install', '-y', '--no-upgrade', '--no-remove', '--no-install-recommends',
               'build-essential', 'meson', 'ninja-build', 'pkg-config', 'udev', 'libudev-dev',
               'gobject-introspection', 'libgirepository1.0-dev', 'dpkg-dev', 'ccache',
               *[name + '=' + versions[package] for name, package in development.items()]])
        if debian_abi(builder) != target_abi:
            raise ValueError('Fingerprint builder dependencies changed the target runtime ABI')
        guest(['meson', 'setup', '/var/tmp/sunuefi-libfprint/build',
               '/var/tmp/sunuefi-libfprint/libfprint', '--prefix=/usr',
               '--libdir=lib/aarch64-linux-gnu', '--buildtype=release',
               '-Ddoc=false', '-Dgtk-examples=false', '-Dinstalled-tests=false',
               '-Dudev_rules_dir=/usr/lib/udev/rules.d', '-Dudev_hwdb=disabled'])
        guest(['meson', 'compile', '-C', '/var/tmp/sunuefi-libfprint/build',
               '-j' + str(package_build.default_jobs())])
        guest(['env', 'DESTDIR=/var/tmp/sunuefi-libfprint/package',
               'meson', 'install', '-C', '/var/tmp/sunuefi-libfprint/build'])
        package = work / 'package'
        shutil.rmtree(package / 'usr/include', ignore_errors=True)
        shutil.rmtree(package / 'usr/lib/aarch64-linux-gnu/pkgconfig', ignore_errors=True)
        control = package / 'DEBIAN'
        control.mkdir()
        copyright_directory = package / 'usr/share/doc/libfprint-2-2'
        copyright_directory.mkdir(parents=True, exist_ok=True)
        for name in LICENSES:
            shutil.copy2(backend / name, copyright_directory / ('piano-goodix-' + name))
        shutil.copy2(backend / 'qcbor/LICENSE', copyright_directory / 'piano-goodix-QCBOR.txt')
        shutil.copy2(backend / 'uapi/linux/tee.h', copyright_directory / 'piano-goodix-linux-tee.h')
        library = package / 'usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0'
        elf = library.read_bytes()
        if elf[:6] != b'\x7fELF\x02\x01' or elf[18:20] != b'\xb7\x00':
            raise ValueError('libfprint must be an ARM64 ELF')
        packaging = work / 'libfprint/debian'
        packaging.mkdir(exist_ok=True)
        (packaging / 'control').write_text(
            'Source: libfprint\nSection: libs\nPriority: optional\n'
            'Maintainer: Project SunUEFI <sunuefi@users.noreply.github.com>\n\n'
            'Package: libfprint-2-2\nArchitecture: any\nDescription: Piano Goodix support\n')
        dependencies = shlib_dependencies(guest([
            'sh', '-ec', 'cd /var/tmp/sunuefi-libfprint/libfprint; '
            'dpkg-shlibdeps -O -e../package/usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0',
        ], capture_output=True, text=True).stdout)
        package_version = overlay_release.debian_version('1:' + cfg['version'] + '+sunuefi2')
        (control / 'control').write_text(
            f'Package: libfprint-2-2\nVersion: {package_version}\nArchitecture: arm64\n'
            'Multi-Arch: same\nSection: libs\nPriority: optional\n'
            'Maintainer: Project SunUEFI <sunuefi@users.noreply.github.com>\n'
            f'Depends: {dependencies}\n'
            'Description: libfprint with native Piano Goodix support\n')
        packages = output / 'runtime'
        packages.mkdir(exist_ok=True)
        deb = packages / f'libfprint-2-2_{package_version.split(":", 1)[-1]}_arm64.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', package, deb], check=True)
        debian_package.verify_build_archive(builder)
    if debian_abi(root) != target_abi:
        raise ValueError('Fingerprint target ABI changed during package compilation')
    if source_inputs() != inputs:
        raise ValueError('Fingerprint sources changed during package compilation')
    # A completed binary remains reusable if installation is interrupted.
    (output / 'SOURCE.json').write_text(json.dumps({
        'inputs': inputs, 'target_abi': target_abi, 'dependencies': dependencies,
        'backend_sources_sha256': digest(backend_archive),
        'package': {'file': deb.name, 'sha256': digest(deb), 'version': package_version,
                    'architecture': 'arm64'},
    }, indent=2) + '\n')
    shutil.rmtree(work)
    install_debian_package(root, deb, target_abi)
