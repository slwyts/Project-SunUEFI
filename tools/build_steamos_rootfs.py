#!/usr/bin/env python3
"""Assemble Valve's ARM64 userspace with the shared Piano BSP and boot files."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

from build_nyarch_apps import guest_environment
from build_release_rootfs import build_resolver, digest
from prepare_steamos_rootfs import prepare, download
import build_arch_package
import build_arch_ffmpeg
import stage_arch_piano
import prepare_steam_client
import prepare_steam_runtime
import build_steamos_gamescope
import build_steamos_manager
import build_piano_runtime_helpers as runtime_helpers
import build_release_helpers

ROOT = Path(__file__).resolve().parents[1]


def configure_repositories(root):
    """Use Valve's public rolling repositories with the image's native DBPath."""
    path = root / 'etc/pacman.conf'
    options = path.read_text().split('[options]', 1)[1].split('\n[', 1)[0]
    public = 'https://holo-packages.steamos.cloud/archlinux-deckard/archlinux/main'
    # The distributed image contains build-specific snapshot endpoints. They
    # are not the public update channel and must not enter our shipped rootfs.
    path.write_text('[options]' + options.rstrip() + '\n\n' + ''.join(
        f'[{repo}]\nServer = {public}/$repo/os/$arch\n\n'
        for repo in ('core', 'extra')))


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def sensor_sources(output, cfg):
    source = ROOT / cfg['source']
    commit = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if commit != cfg['commit'] or subprocess.check_output(
            ['git', '-C', source, 'status', '--porcelain'], text=True):
        raise ValueError('Run ./build.sh sources to obtain the fixed sensor source')
    shutil.copytree(source, output, ignore=shutil.ignore_patterns('.git'))
    rows = []
    for name in cfg['patches']:
        run('patch', '--batch', '--forward', '-p1', '-i', ROOT / name, cwd=output)
        rows.append({'path': name, 'sha256': digest(ROOT / name)})
    (output / 'SOURCE').write_text(json.dumps(
        {'source_url': cfg['source_url'], 'source_commit': commit, 'patches': rows}, indent=2) + '\n')


def sensor_recipes(output, source):
    for name in ('fastrpc', 'libssc', 'iio-sensor-proxy'):
        recipe = output / name
        shutil.copytree(ROOT / 'linux/bsp/arch' / name, recipe)
        if name == 'fastrpc':
            continue
        if name == 'libssc':
            filename, pool, checksum = (
                'libssc_0.4.4.orig.tar.gz', 'libs/libssc',
                '716d6bd6b34d2d753060c6b54c9a87e34fae75b724c763bf9ef487efa3621587')
        else:
            filename, pool, checksum = (
                'iio-sensor-proxy_3.9.orig.tar.bz2', 'i/iio-sensor-proxy',
                '19a4ae99b1b8af603fc789841025f9747bf445418d69ec7389e75adb6c8186bf')
        download('https://deb.debian.org/debian/pool/main/' + pool + '/' + filename,
                 recipe / filename, checksum)
        for patch in (source / 'patches' / name).glob('*.patch'):
            shutil.copy2(patch, recipe / patch.name)


def kde_recipes(output, root):
    """Build Qt-sensitive desktop components against Valve's current Qt ABI."""
    packages = json.loads((ROOT / 'config/steamos.json').read_text())['kde_packages']
    qt_version = subprocess.check_output(
        ['chroot', root, 'pacman', '-Q', 'qt6-base'], text=True).split()[1]
    for item in packages:
        folder = output / item['name']
        folder.mkdir(parents=True, exist_ok=True)
        q = shlex.quote
        patches = [ROOT / p for p in item.get('patches', [])]
        for patch in patches:
            shutil.copy2(patch, folder / patch.name)
        sources = [item['url'], *[p.name for p in patches]]
        checksums = [item['sha256'], *[digest(p) for p in patches]]
        prepare_commands = ''.join(
            f'  patch --batch --fuzz=0 -p1 -i "$srcdir/{p.name}"\n'
            for p in patches)
        license = ' '.join(q(p) for p in item.get(
            'license', ['GPL-2.0-or-later', 'LGPL-2.1-or-later']))
        (folder / 'PKGBUILD').write_text(
            f"# Qt ABI used for this build: {qt_version}\n"
            f"pkgname={q(item['name'])}\npkgver={q(item['version'])}"
            f"\npkgrel={q(item.get('pkgrel', '1.2'))}\n"
            "pkgdesc='KDE desktop component for SteamOS ARM64'\narch=(aarch64)\n"
            f"url='https://kde.org/'\nlicense=({license})\n"
            f"depends=({' '.join(q(p) for p in item['depends'])})\n"
            f"makedepends=(base-devel cmake ninja extra-cmake-modules kdoctools libglvnd"
            f"{''.join(' ' + q(p) for p in item.get('makedepends', []))})\n"
            "options=(!lto !debug)\n"
            f"source=({' '.join(q(p) for p in sources)})\n"
            f"sha256sums=({' '.join(q(p) for p in checksums)})\n"
            + ('prepare() {\n  cd "$pkgname-$pkgver"\n'
               + prepare_commands + '}\n' if patches else '') +
            'build() {\n'
            '  local opengl_libdir glx_libdir qt_libdir\n'
            '  opengl_libdir=$(pkg-config --variable=libdir opengl)\n'
            '  glx_libdir=$(pkg-config --variable=libdir glx)\n'
            '  test -r "$opengl_libdir/libOpenGL.so"\n'
            '  test -r "$glx_libdir/libGLX.so"\n'
            '  qt_libdir=$(pkg-config --variable=libdir Qt6Core)\n'
            '  test -r "$qt_libdir/cmake/Qt6/qt.toolchain.cmake"\n'
            '  cmake -S "$pkgname-$pkgver" -B build -G Ninja '
            '-DCMAKE_TOOLCHAIN_FILE="$qt_libdir/cmake/Qt6/qt.toolchain.cmake" '
            '-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr '
            '-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_LIBEXECDIR=lib '
            '-DCMAKE_LIBRARY_PATH="$opengl_libdir;$glx_libdir;$qt_libdir" '
            '-DOPENGL_opengl_LIBRARY="$opengl_libdir/libOpenGL.so" '
            '-DOPENGL_glx_LIBRARY="$glx_libdir/libGLX.so" '
            '-DBUILD_TESTING=OFF -DBUILD_QCH=OFF\n'
            '  cmake --build build -j4\n}\n'
            'package() {\n  DESTDIR="$pkgdir" cmake --install build\n'
            '  install -d "$pkgdir/usr/share/licenses/$pkgname"\n'
            '  cp -a "$pkgname-$pkgver/LICENSES/." "$pkgdir/usr/share/licenses/$pkgname/"\n}\n')
    return [item['name'] for item in packages]


def kde_package_is_current(package_dir, recipe):
    manifest = package_dir / 'manifest.json'
    if not manifest.is_file():
        return False
    try:
        record = json.loads(manifest.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    return (record.get('recipe_sha256') == digest(recipe / 'PKGBUILD') and
            all((package_dir / name).is_file() and
                digest(package_dir / name) == row.get('sha256')
                for name, row in record.get('packages', {}).items()) and
            bool(record.get('packages')))


def build(args):
    output = args.output.resolve()
    if os.geteuid() or not output.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Run as root with output below build/distros')
    if output.exists() and not args.resume:
        raise ValueError('Output exists; use --resume only for this same build')
    output.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'config/release.json').read_text())
    base_config = json.loads((ROOT / 'config/steamos.json').read_text())['base']
    base = ROOT / 'build' / ('steamos-base-' + base_config['version'])
    base_record = prepare(base, workers=32)
    root = output / 'rootfs'
    if not root.exists():
        mount = output / 'base-mount'
        mount.mkdir()
        run('mount', '-o', 'loop,ro', base / 'rootfs.img', mount)
        try:
            root.mkdir()
            run('rsync', '-aHAX', '--numeric-ids', '--filter=-x btrfs.*', str(mount) + '/', root)
        finally:
            run('umount', mount)
        (output / 'BASE.json').write_text(json.dumps(base_record, indent=2) + '\n')
    elif not (output / 'BASE.json').is_file():
        raise ValueError('Existing root has no verified base record')
    if json.loads((output / 'BASE.json').read_text())['base'] != base_record['base']:
        raise ValueError('Existing root belongs to another Valve base image')

    configure_repositories(root)
    # Keep the native pacman ABI and separate FEX guest runtime. Native Mesa
    # is rebuilt below because the Frame image does not know Piano's A830v1.
    with build_resolver(root), guest_environment(root) as guest:
        # SunUEFI supplies the kernel, initramfs and matching loopback module.
        # Remove the headset kernel through pacman so later package updates
        # do not rebuild unused Frame initramfs images.
        installed = subprocess.check_output(
            ['chroot', root, 'pacman', '-Qq'], text=True).splitlines()
        frame_kernels = [p for p in installed
                         if re.fullmatch(r'linux-[0-9]+-(?:v4l2loopback-)?deckard', p)]
        if frame_kernels:
            guest(['pacman', '-R', '--noconfirm', *frame_kernels])
        # Refresh the complete Valve package cohort before building local
        # adapters against it. Partial package upgrades can mix library ABIs.
        # Frame split these files into clang-libs; the public repository now
        # ships them in clang itself. Remove that obsolete split package before
        # the full transaction, which installs the current compiler and libs.
        for obsolete in ('clang-libs', 'deckard-steamvr-session'):
            if subprocess.run(['chroot', root, 'pacman', '-Qq', obsolete],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                # The VR session also claims plasma.desktop, now supplied by
                # the ordinary Plasma package used on the tablet.
                guest(['pacman', '-Rdd', '--noconfirm', obsolete])
        # Frame's prebuilt locale subset pins its image's exact glibc version
        # and is absent from the public repository. Replace it with the public
        # locale package in the full upgrade, keeping glibc and locales matched.
        if subprocess.run(['chroot', root, 'pacman', '-Qq', 'holo-glibc-locales'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            guest(['pacman', '-R', '--noconfirm', 'holo-glibc-locales'])
        guest(['pacman', '-Syu', '--noconfirm', 'glibc-locales'])
        packages = ['base-devel',
               'sddm', 'openssh', 'bluez', 'bluez-utils', 'networkmanager', 'wpa_supplicant',
               'alsa-utils', 'pipewire', 'pipewire-pulse', 'wireplumber',
               'python', 'python-gobject', 'polkit', 'erofs-utils', 'rsync', 'chromium',
               'protobuf-c', 'libglvnd',
               'bluez-qt', 'networkmanager-qt', 'modemmanager-qt',
               'bluedevil', 'plasma-nm', 'kinfocenter', 'powerdevil',
               'discover', 'packagekit', 'packagekit-qt6', 'pacman-contrib',
               'dconf', 'noto-fonts-cjk']
        # Some Frame packages, including SDDM, are shipped in the base but
        # absent from the public repositories. Syu already refreshes packages
        # available there; request only unmet dependencies in this step.
        missing = subprocess.run(['chroot', root, 'pacman', '-T', *packages],
                                 capture_output=True, text=True)
        if missing.returncode not in (0, 127):
            raise RuntimeError(missing.stderr)
        if missing.returncode:
            guest(['pacman', '-S', '--needed', '--noconfirm',
                   *missing.stdout.splitlines()])
        # Valve includes the Wayland modules in qt6-base. Ask pacman to check
        # the complete Qt cohorts rather than checking one CMake file at a time.
        for package in ('qt6-base', 'qt6-declarative'):
            if subprocess.run(['chroot', root, 'pacman', '-Qk', package]).returncode:
                guest(['pacman', '-S', '--noconfirm', package])
            guest(['pacman', '-Qk', package])
        # Resolve inside the target root, including absolute symlink targets.
        # KDE recipes pass these pkg-config paths to Qt's CMake dependency scan.
        opengl_check = ['bash', '-euc', '''
for module in opengl glx; do
    directory=$(pkg-config --variable=libdir "$module")
    case "$module" in opengl) library=libOpenGL.so ;; glx) library=libGLX.so ;; esac
    test -r "$directory/$library"
done
''']
        if subprocess.run(['chroot', root, *opengl_check]).returncode:
            guest(['pacman', '-S', '--noconfirm', 'libglvnd'])
        guest(opengl_check)
        if not (root / 'usr/share/ibus/component/pinyin.xml').is_file():
            raise ValueError('The Valve base image must include its native IBus Pinyin engine')
        if subprocess.run(['chroot', root, 'id', 'piano'], capture_output=True).returncode:
            guest(['usermod', '-l', 'piano', '-d', '/home/piano', '-m', 'steamos'])
            guest(['groupmod', '-n', 'piano', 'steamos'])
        # Keep Arch's standard account policy; the password is documented in
        # README and shared with the other images.
        guest(['usermod', '-aG', 'wheel', 'piano'])
        # Valve's standard restricted sysfs writer grants the deck group
        # access to backlight controls after its polkit check.
        guest(['groupadd', '-r', '-f', 'deck'])
        guest(['usermod', '-aG', 'deck', 'piano'])
        locale = root / 'etc/locale.gen'
        locale.write_text('en_US.UTF-8 UTF-8\nzh_CN.UTF-8 UTF-8\n')
        guest(['locale-gen'])
    for name in kde_recipes(output / 'kde-recipes', root):
        recipe = output / 'kde-recipes' / name
        packages = output / 'packages' / name
        if not kde_package_is_current(packages, recipe):
            for stale in packages.glob('*.pkg.tar.*'):
                stale.unlink()
            build_arch_package.build(root, recipe, packages)
        else:
            build_arch_package.install_cached(root, packages)
    power_profiles = output / 'packages/power-profiles-daemon'
    if not (power_profiles / 'manifest.json').is_file():
        build_arch_package.build(
            root, ROOT / 'linux/bsp/steamos/power-profiles-daemon', power_profiles)
    source = output / 'sensor-source'
    if not source.exists():
        sensor_sources(source, cfg['sensors'])
    recipes = output / 'sensor-recipes'
    if not recipes.exists():
        sensor_recipes(recipes, source)
    for name in ('fastrpc', 'libssc', 'iio-sensor-proxy'):
        packages = output / 'packages' / name
        if not (packages / 'manifest.json').is_file():
            build_arch_package.build(root, recipes / name, packages)
    mesa = output / 'packages/mesa'
    if not (mesa / 'manifest.json').is_file():
        recipe = output / 'mesa-recipe'
        shutil.copytree(ROOT / 'linux/bsp/steamos/mesa', recipe, dirs_exist_ok=True)
        shutil.copy2(ROOT / 'upstream/piano-mesa-current/patches/0001-freedreno-Add-chip-id-support-for-A830v1.patch',
                     recipe)
        build_arch_package.build(root, recipe, mesa)
    gamescope = output / 'packages/gamescope'
    manager = output / 'packages/steamos-manager'
    if not build_steamos_manager.is_current(manager):
        if manager.exists():
            shutil.rmtree(manager)
        build_steamos_manager.build(root, manager)
    if not build_steamos_gamescope.is_current(gamescope):
        if gamescope.exists():
            shutil.rmtree(gamescope)
        build_steamos_gamescope.build(root, gamescope)
    ffmpeg = output / 'packages/ffmpeg'
    if not (ffmpeg / 'SOURCE.json').is_file():
        build_arch_ffmpeg.build(root, ffmpeg, resume=ffmpeg.exists())
    client = output / 'steam-client'
    if not (client / 'SOURCE.json').is_file():
        prepare_steam_client.prepare(root, client)
    if json.loads((client / 'SOURCE.json').read_text()) != json.loads(
            (ROOT / 'config/steam-client.json').read_text()):
        raise ValueError('Cached Steam client differs from config/steam-client.json')
    prepare_steam_client.enable_first_launch(root)
    game_runtime = output / 'steam-runtime'
    if not (game_runtime / 'SOURCE.json').is_file():
        prepare_steam_runtime.prepare(root, game_runtime)
    runtime_record = json.loads((game_runtime / 'SOURCE.json').read_text())
    if runtime_record['config_sha256'] != digest(ROOT / 'config/steam-runtime.json'):
        raise ValueError('Cached Proton/runtime differs from config/steam-runtime.json')

    kernel = ROOT / 'artifacts/kernels/release'
    kernel_source = Path(json.loads((ROOT / 'build/release/source-manifest.json').read_text())['worktree'])
    runtime = output / 'runtime'
    kernel_record = json.loads((kernel / 'manifest.json').read_text())
    runtime_kernel = runtime_helpers.kernel_identity(
        args.kernel_build.resolve(), kernel_source,
        kernel_record['source_commit'], kernel_record['kernel_release'])
    if (runtime / 'manifest.json').is_file():
        old = json.loads((runtime / 'manifest.json').read_text())
        if old['kernel'] != runtime_kernel or not build_release_helpers.inputs_current(runtime):
            shutil.rmtree(runtime)
    if not (runtime / 'manifest.json').exists():
        from build_piano_full_kernel import toolchain
        helper_env, helper_tools = toolchain()
        run(sys.executable, ROOT / 'tools/build_release_helpers.py',
            '--kernel', kernel, '--source', kernel_source,
            '--kernel-build', args.kernel_build.resolve(),
            '--output', runtime, '--sysroot', args.bootstrap_sysroot.resolve(),
            '--cc', helper_tools['paths']['clang'], env=helper_env)
    # Shared stager installs firmware, matching modules, standard interfaces,
    # input/audio/camera helpers, suspend policy and the LABEL-based initramfs.
    stage_arch_piano.stage(argparse.Namespace(
        rootfs=root, kernel=kernel, runtime=runtime, sensor_source=source,
        bootstrap_sysroot=args.bootstrap_sysroot, desktop='steamos'))
    manifest = output / 'manifest.json'
    record = json.loads(manifest.read_text())
    record.update(base_source=base_record, ffmpeg=json.loads((ffmpeg / 'SOURCE.json').read_text()),
                  steam_client=json.loads((client / 'SOURCE.json').read_text()),
                  steam_runtime=runtime_record)
    manifest.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/distros/steamos-piano')
    parser.add_argument('--bootstrap-sysroot', type=Path, default=Path('/'),
                        help='Native ARM64 Debian build environment containing busybox-static')
    parser.add_argument('--kernel-build', type=Path, default=ROOT / 'build/kernels/release',
                        help='Build directory matching artifacts/kernels/release')
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(run('unshare', '--mount', '--propagation', 'private',
                             sys.executable, __file__, *sys.argv[1:], '--worker').returncode)
    build(args)
