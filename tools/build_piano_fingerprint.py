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
    sources = json.loads((ROOT / 'config/fingerprint.json').read_text())['sources']
    patch_inputs = {}
    for record in sources.values():
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


def build(output, flags, emulator=None):
    inputs = source_inputs()
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())
    dependencies = {name: source(name, record)
                    for name, record in cfg['sources'].items()}
    work = output / 'fingerprint'
    work.mkdir()
    for name, record in cfg['sources'].items():
        if not patches(record):
            continue
        # Extract the fixed upstream commit; apply changes only in the build tree.
        destination, archive = work / name, work / (name + '.tar')
        destination.mkdir()
        subprocess.run(['git', '-C', dependencies[name], 'archive', '--format=tar',
                        '--output=' + str(archive.resolve()), record['commit']], check=True)
        subprocess.run(['tar', '-xf', archive, '-C', destination], check=True)
        archive.unlink()
        for path, _expected in patches(record):
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', path],
                           cwd=destination, check=True)
        dependencies[name] = destination
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
        *[NATIVE / name for name in CORE_SOURCES],
        CLI_SOURCE,
    ]
    objects = []
    for index, original in enumerate(originals):
        obj = work / (str(index) + '.o')
        command = [*flags, '-fPIC', '-D_GNU_SOURCE', '-DUSE_QCBOR',
                   *['-I' + str(p) for p in includes],
                   '-c', original, '-o', obj]
        # Imported Qualcomm sources retain their own warning policy.
        if not original.is_relative_to(NATIVE):
            command = [arg for arg in command if arg != '-Werror']
        subprocess.run(command, check=True)
        objects.append(obj)
    archiver = shutil.which('llvm-ar') or shutil.which('ar')
    if not archiver:
        raise ValueError('Fingerprint desktop integration requires ar')
    library = work / 'libpiano-goodix.a'
    subprocess.run([archiver, 'rcs', library, *objects[:-1]], check=True)
    shutil.copy2(NATIVE / 'piano-goodix.h', work / 'piano-goodix.h')
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


def desktop_recipes(output, runtime):
    """Package standard libfprint/fprintd against the target distro ABI."""
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']
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
        local = ['libpiano-goodix.a', 'piano-goodix.h'] if name == 'libfprint' else []
        for filename in local:
            shutil.copy2(runtime / 'fingerprint' / filename, recipe / filename)
        files = [archive.name, *patch_names, *local]
        quoted = lambda values: ' '.join(shlex.quote(str(value)) for value in values)
        dependencies = (['glib2', 'libgusb', 'libgudev', 'pixman', 'openssl']
                        if name == 'libfprint' else ['libfprint', 'dbus', 'dbus-glib', 'polkit', 'pam'])
        prepare = ''.join('  patch -d "$srcdir/' + name + '" -p1 < "$srcdir/' + p + '"\n'
                          for p in patch_names)
        sdk = ''
        options = ('-Ddoc=false -Dgtk-examples=false -Dinstalled-tests=false '
                   '-Dudev_rules_dir=/usr/lib/udev/rules.d -Dudev_hwdb=disabled'
                   if name == 'libfprint' else '-Dgtk_doc=false -Dman=false')
        if name == 'libfprint':
            sdk = '''  mkdir -p "$srcdir/sdk/lib/pkgconfig" "$srcdir/sdk/include"
  cp "$srcdir/libpiano-goodix.a" "$srcdir/sdk/lib/"
  cp "$srcdir/piano-goodix.h" "$srcdir/sdk/include/"
  cat > "$srcdir/sdk/lib/pkgconfig/piano-goodix.pc" <<EOF
Name: piano-goodix
Description: Piano signed-TA fingerprint transport
Version: 1.0
Libs: -L$srcdir/sdk/lib -lpiano-goodix -pthread -lm
Cflags: -I$srcdir/sdk/include
EOF
  export PKG_CONFIG_PATH="$srcdir/sdk/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
'''
        recipe_text = (
            f'pkgname={name}\npkgver={record["version"]}\npkgrel=1.1\n'
            'arch=(aarch64)\nlicense=(LGPL-2.1-or-later)\n'
            f'pkgdesc="Standard {name} with Piano fingerprint support"\n'
            f'url={record["url"]}\ndepends=({quoted(dependencies)})\n'
            'makedepends=(base-devel meson ninja gobject-introspection python-docutils perl)\n'
            'options=(!lto !debug)\n'
            + ('noextract=(libpiano-goodix.a)\n' if name == 'libfprint' else '') +
            f'source=({quoted(files)})\nsha256sums=({quoted(digest(recipe / f) for f in files)})\n'
            'prepare() {\n' + (prepare or '  :\n') + '}\n'
            'build() {\n' + sdk + f'  meson setup build {name} --prefix=/usr --libdir=lib '
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
        if not (packages / 'manifest.json').is_file():
            build_arch_package.build(root, recipes / name, packages)
        else:
            build_arch_package.install_cached(root, packages)
    desktop_service(root)


def desktop_service(root):
    config = root / 'usr/lib/systemd/system/fprintd.service.d/20-piano.conf'
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text('[Service]\nStateDirectory=piano-goodix\nStateDirectoryMode=0700\n'
                      'DeviceAllow=char-uio rw\nDeviceAllow=char-tee rw\nDeviceAllow=char-bsg rw\n')


def install_debian_desktop(root, runtime, output):
    from build_release_rootfs import build_devices, build_resolver
    from host import package_build
    cfg = json.loads((ROOT / 'config/fingerprint.json').read_text())['desktop']['libfprint']
    output.mkdir(parents=True, exist_ok=True)
    work = root / 'var/tmp/sunuefi-libfprint'
    if work.exists():
        raise ValueError('Inspect the existing fingerprint package build before retrying')
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
    sdk = work / 'sdk'
    (sdk / 'lib/pkgconfig').mkdir(parents=True)
    (sdk / 'include').mkdir()
    for name, folder in (('libpiano-goodix.a', 'lib'), ('piano-goodix.h', 'include')):
        shutil.copy2(runtime / 'fingerprint' / name, sdk / folder / name)
    (sdk / 'lib/pkgconfig/piano-goodix.pc').write_text(
        'Name: piano-goodix\nDescription: Piano signed-TA fingerprint transport\nVersion: 1.0\n'
        'Libs: -L/var/tmp/sunuefi-libfprint/sdk/lib -lpiano-goodix -pthread -lm\n'
        'Cflags: -I/var/tmp/sunuefi-libfprint/sdk/include\n')
    with build_devices(root), build_resolver(root), package_build.environment(root) as settings:
        def guest(args):
            subprocess.run(['chroot', root, 'env', *package_build.arguments(settings), *args], check=True)

        guest(['apt-get', 'install', '-y', '--no-install-recommends',
               'build-essential', 'meson', 'ninja-build', 'pkg-config', 'udev', 'libudev-dev',
               'libglib2.0-dev', 'libgusb-dev', 'libgudev-1.0-dev',
               'libpixman-1-dev', 'libssl-dev', 'libgirepository1.0-dev',
               'fprintd', 'libpam-fprintd', 'ccache'])
        guest(['env',
               'PKG_CONFIG_PATH=/var/tmp/sunuefi-libfprint/sdk/lib/pkgconfig',
               'meson', 'setup', '/var/tmp/sunuefi-libfprint/build',
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
        (control / 'control').write_text(
            f'Package: libfprint-2-2\nVersion: 1:{cfg["version"]}+sunuefi1\nArchitecture: arm64\n'
            'Maintainer: Project SunUEFI\nDepends: libc6, libglib2.0-0t64, libgusb2, '
            'libgudev-1.0-0, libpixman-1-0, libssl3t64\n'
            'Description: libfprint with native Piano Goodix support\n')
        deb = output / f'libfprint-2-2_{cfg["version"]}+sunuefi1_arm64.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', package, deb], check=True)
        shutil.copy2(deb, work / deb.name)
        subprocess.run(['chroot', root, 'env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                        'dpkg', '-i', '/var/tmp/sunuefi-libfprint/' + deb.name], check=True)
    desktop_service(root)
    shutil.rmtree(work)
