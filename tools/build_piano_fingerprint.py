#!/usr/bin/env python3
"""Build the native Goodix client against fixed, public Qualcomm sources."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import shlex

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / 'linux/userspace/fingerprint'
CORE_SOURCES = (
    'piano-goodix-storage.c', 'piano-goodix-events.c',
    'piano-goodix-gatekeeper.c', 'piano-goodix-operations.c',
    'piano-goodix-rpmb.c', 'piano-goodix.c',
)
HEADERS = (
    'piano-goodix-storage.h', 'piano-goodix-events.h',
    'piano-goodix-gatekeeper.h', 'piano-goodix-operations.h',
    'piano-goodix-rpmb.h', 'piano-goodix.h',
)
CLI_SOURCE = NATIVE / 'piano-goodix-cli.c'
RUNTIME_PATHS = {
    'usr/local/sbin/piano-fingerprint-ctl',
    'usr/share/piano-provenance/fingerprint.json',
    *{f'usr/share/licenses/piano-fingerprint/{name}.txt'
      for name in ('quic-teec', 'minkipc', 'qcbor')},
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
                           for name in (*CORE_SOURCES, *HEADERS, CLI_SOURCE.name)},
        'builder_sha256': digest(Path(__file__)),
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


def stage_sources(work):
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
    for name in (*CORE_SOURCES, *HEADERS):
        shutil.copy2(NATIVE / name, work / name)
    return dependencies


def build(output, flags, emulator=None):
    inputs = source_inputs()
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())
    work = output / 'fingerprint'
    work.mkdir()
    backend = work / 'backend'
    dependencies = stage_sources(backend)
    qtee, mink, qcbor = (dependencies[name] for name in ('quic-teec', 'minkipc', 'qcbor'))
    includes = [qtee / 'libqcomtee/include', qtee / 'libqcomtee/src',
                mink / 'libminkadaptor/include', mink / 'libminkadaptor/src',
                mink / 'listeners/libfsservice/gpfs', mink / 'listeners/libfsservice/fs',
                mink / 'listeners/libfsservice/cmn',
                qcbor / 'inc']
    originals = [
        qtee / 'libqcomtee/src/qcomtee_object.c',
        qtee / 'libqcomtee/src/objects/credentials_obj.c',
        qtee / 'libqcomtee/src/objects/mem_obj.c',
        mink / 'libminkadaptor/src/mink_adaptor.c',
        mink / 'libminkadaptor/src/supplicant.c',
        mink / 'libminkadaptor/src/syscall.S',
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
    backend_archive = work / 'piano-goodix-sources.tar'
    subprocess.run(['tar', '--sort=name', '--mtime=@0', '--owner=0', '--group=0',
                    '--numeric-owner', '-cf', backend_archive, '-C', backend, '.'], check=True)
    binary = work / 'piano-fingerprint-ctl'
    subprocess.run([*flags, '-static', *objects, '-pthread', '-lm', '-o', binary], check=True)
    subprocess.run(([emulator] if emulator else []) + [str(binary), '--help'], check=True)
    files = {}
    for name, path, mode in [
            ('usr/local/sbin/piano-fingerprint-ctl', binary, 0o755),
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
        'backend_archive': {'file': str(backend_archive.relative_to(output)),
                            'sha256': digest(backend_archive)},
        'firmware': json.loads(
            (ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json').read_text())['files'],
        'operations': ['info', 'initialize', 'prepare-auth', 'watch', 'list',
                       'storage-probe', 'enroll', 'verify', 'delete'],
    }
    if source_inputs() != inputs:
        raise ValueError('Fingerprint sources changed during compilation')
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


def desktop_recipes(output, runtime):
    """Build the local libfprint backend and fprintd against the target distro ABI."""
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']
    backend = backend_sources(runtime)
    output.mkdir(parents=True, exist_ok=True)
    for name, record in cfg.items():
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
        quoted = lambda values: ' '.join(shlex.quote(str(value)) for value in values)
        dependencies = (['glib2', 'libgusb', 'libgudev', 'pixman', 'openssl']
                        if name == 'libfprint' else ['libfprint', 'dbus', 'dbus-glib', 'polkit', 'pam'])
        prepare = ''.join('  patch -d "$srcdir/' + name + '" -p1 < "$srcdir/' + p + '"\n'
                          for p in patch_names)
        options = ('-Ddoc=false -Dgtk-examples=false -Dinstalled-tests=false '
                   '-Dudev_rules_dir=/usr/lib/udev/rules.d -Dudev_hwdb=disabled'
                   if name == 'libfprint' else '-Dgtk_doc=false -Dman=false')
        if name == 'libfprint':
            prepare += '''  mkdir -p "$srcdir/libfprint/libfprint/drivers/piano-goodix"
  tar -xf "$srcdir/piano-goodix-sources.tar" -C "$srcdir/libfprint/libfprint/drivers/piano-goodix"
'''
        recipe_text = (
            f'pkgname={name}\npkgver={record["version"]}\npkgrel=1.1\n'
            'arch=(aarch64)\nlicense=(LGPL-2.1-or-later)\n'
            f'pkgdesc="{name} with Piano fingerprint support"\n'
            f'url={record["url"]}\ndepends=({quoted(dependencies)})\n'
            'makedepends=(base-devel meson ninja gobject-introspection python-docutils perl)\n'
            'options=(!lto !debug)\n'
            + ('noextract=(piano-goodix-sources.tar)\n' if name == 'libfprint' else '') +
            f'source=({quoted(files)})\nsha256sums=({quoted(digest(recipe / f) for f in files)})\n'
            'prepare() {\n' + (prepare or '  :\n') + '}\n'
            'build() {\n' + f'  meson setup build {name} --prefix=/usr --libdir=lib '
            f'--buildtype=release {options}\n  meson compile -C build -j4\n}}\n'
            'package() {\n  DESTDIR="$pkgdir" meson install -C build\n}\n')
        (recipe / 'PKGBUILD').write_text(recipe_text)
    return cfg


def install_desktop(root, runtime, output):
    if (root / 'var/lib/dpkg/status').is_file():
        return install_debian_desktop(root, runtime, output)
    import build_arch_package
    recipes = output / 'recipes'
    desktop_recipes(recipes, runtime)
    for name in ('libfprint', 'fprintd'):
        packages = output / name
        cohort = subprocess.check_output(
            ['chroot', root, 'pacman', '-Q', 'glibc', 'glib2', 'libgusb', 'libgudev',
             'pixman', 'openssl'], text=True)
        inputs = {'recipe_sha256': digest(recipes / name / 'PKGBUILD'),
                  'runtime_packages': cohort}
        if not (packages / 'manifest.json').is_file():
            build_arch_package.build(root, recipes / name, packages)
            updated = subprocess.check_output(
                ['chroot', root, 'pacman', '-Q', 'glibc', 'glib2', 'libgusb', 'libgudev',
                 'pixman', 'openssl'], text=True)
            if updated != cohort:
                raise ValueError('Fingerprint build dependencies changed the target ABI; rebuild the packages')
            (packages / 'fingerprint-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        else:
            marker = packages / 'fingerprint-inputs.json'
            if not marker.is_file() or json.loads(marker.read_text()) != inputs:
                raise ValueError('Fingerprint package sources or target ABI changed; use a fresh output')
            build_arch_package.install_cached(root, packages)
    desktop_service(root)


def desktop_service(root):
    config = root / 'usr/lib/systemd/system/fprintd.service.d/20-piano.conf'
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[Service]\nStateDirectory=piano-goodix\nStateDirectoryMode=0700\n'
                      'DeviceAllow=char-uio rw\nDeviceAllow=char-tee rw\nDeviceAllow=char-bsg rw\n')


DEBIAN_ABI_PACKAGES = (
    'libc6', 'libglib2.0-0t64', 'libgusb2', 'libgudev-1.0-0', 'libpixman-1-0', 'libssl3t64',
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


def install_debian_desktop(root, runtime, output):
    from build_release_rootfs import build_devices, build_resolver
    from desktop import package_build
    root, runtime, output = root.resolve(), runtime.resolve(), output.resolve()
    if (not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root)):
        raise ValueError('Build fingerprint packages outside a project-built target rootfs')
    output.mkdir(parents=True, exist_ok=True)
    builder = output / 'builder/rootfs'
    if builder.parent.exists():
        raise ValueError('Inspect the existing fingerprint builder before retrying with a fresh output')
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']['libfprint']
    backend_archive = backend_sources(runtime)
    debian_abi(root)
    # Runtime packages belong in the image; compilers and -dev packages do not.
    with build_devices(root), build_resolver(root):
        subprocess.run(['chroot', root, 'env', 'DEBIAN_FRONTEND=noninteractive',
                        'PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'apt-get', 'install', '-y',
                        '--no-upgrade', '--no-remove', '--no-install-recommends',
                        'fprintd', 'libpam-fprintd'], check=True)
    target_abi = debian_abi(root)
    builder.parent.mkdir()
    subprocess.run(['cp', '-a', '--reflink=auto', root, builder], check=True)
    work = builder / 'var/tmp/sunuefi-libfprint'
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
            'libgusb-dev': 'libgusb2', 'libgudev-1.0-dev': 'libgudev-1.0-0',
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
        package_version = '1:' + cfg['version'] + '+sunuefi2'
        (control / 'control').write_text(
            f'Package: libfprint-2-2\nVersion: {package_version}\nArchitecture: arm64\n'
            'Multi-Arch: same\nSection: libs\nPriority: optional\n'
            'Maintainer: Project SunUEFI <sunuefi@users.noreply.github.com>\n'
            f'Depends: {dependencies}\n'
            'Description: libfprint with native Piano Goodix support\n')
        deb = output / f'libfprint-2-2_{cfg["version"]}+sunuefi2_arm64.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', package, deb], check=True)
    if debian_abi(root) != target_abi:
        raise ValueError('Fingerprint target ABI changed during package compilation')
    install = root / 'var/tmp/sunuefi-libfprint-install'
    if install.exists():
        raise ValueError('Inspect the existing fingerprint install directory before retrying')
    shutil.copytree(package, install / 'package')
    shutil.copy2(deb, install / deb.name)
    with build_devices(root):
        # Resolve the replacement library and fprintd against the image's own
        # loader before replacing the installed, working backend.
        library_path = '/var/tmp/sunuefi-libfprint-install/package/usr/lib/aarch64-linux-gnu'
        daemon = next((path for path in ('/usr/libexec/fprintd', '/usr/lib/fprintd/fprintd')
                       if (root / path.lstrip('/')).is_file()), None)
        if daemon is None:
            raise ValueError('The target fprintd executable is missing')
        for binary in (library_path + '/libfprint-2.so.2.0.0', daemon):
            result = subprocess.run(['chroot', root, 'env', 'LC_ALL=C',
                                     'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                                     'LD_LIBRARY_PATH=' + library_path, 'ldd', '-r', binary],
                                    capture_output=True, text=True, check=True)
            messages = result.stdout + result.stderr
            if 'not found' in messages or 'undefined symbol' in messages:
                raise ValueError('Fingerprint library or fprintd does not resolve against the target ABI')
        subprocess.run(['chroot', root, 'env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                        'dpkg', '-i', '/var/tmp/sunuefi-libfprint-install/' + deb.name],
                       check=True)
    (output / 'SOURCE.json').write_text(json.dumps({
        'inputs': source_inputs(), 'target_abi': target_abi, 'dependencies': dependencies,
        'backend_sources_sha256': digest(backend_archive),
        'package': {'file': deb.name, 'sha256': digest(deb), 'version': package_version,
                    'architecture': 'arm64'},
    }, indent=2) + '\n')
    desktop_service(root)
    shutil.rmtree(install)
    shutil.rmtree(work)
