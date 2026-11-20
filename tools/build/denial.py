"""Install Denial's signed native packages and configure the tablet session."""
from pathlib import Path
import subprocess
import tempfile

from build import overlay_packages
from package.download import download_debian
from build_piano_ram_bootstrap import guest_resolve


ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = 'packages.denialwm.org'
KEY = ROOT / 'config/keys/denial-repo-key.asc'
FINGERPRINT = 'AE4108FA5E91E26BE0EE331E0F5B3AD16E023091'
APTORIGIN = ('trixie', 'trixie', 'Denial')
ENGINE = 'denial-flutter-engine'
PLUGINS = 'denial-plugin-manager'
PACKAGES = ('denial', ENGINE)
DESKTOP_PACKAGES = ('fcitx5', 'fcitx5-chinese-addons', 'foot', 'sddm', 'thunar', 'upower',
                    'weston', 'wireplumber')
APT_SOURCE = 'sources.list.d/denial.sources'
APT_KEY = '/etc/apt/keyrings/denial.asc'
PACMAN_INCLUDE = 'Include = /etc/pacman.d/denial.conf'
SESSION = 'usr/share/wayland-sessions/denial.desktop'
COMPOSITOR = '/usr/bin/deniald'


def image_manifest(path, target, *, desktop, published):
    if path is None:
        if desktop == 'denial' and published:
            raise ValueError('A published Denial image requires --denial-packages')
        return None
    if desktop != 'denial' or not published:
        raise ValueError('--denial-packages is only for a Denial image with --published-packages')
    from build.denial_manifest import read_manifest

    path = Path(path).resolve()
    read_manifest(path, target)
    return path


def published_set(path, target, installed):
    from build.denial_manifest import read_manifest

    manifest = read_manifest(path, target)
    packages = {filename: row for filename, row in manifest['packages'].items()
                if row['name'] != PLUGINS or PLUGINS in installed}
    engine = manifest['upstream_release']['archives'][ENGINE]
    return packages, engine, manifest['binary_sha256']


def debian_versions(rootfs):
    listing = subprocess.check_output(
        ['chroot', rootfs, 'dpkg-query', '-W', '-f=${Package}\t${Version}\t${db:Status-Status}\n'],
        text=True)
    return {name: version for name, version, status in (line.split('\t') for line in listing.splitlines())
            if status == 'installed'}


def verified_versions(expected, installed):
    for name, version in expected.items():
        actual = installed.get(name)
        if actual != version:
            raise ValueError(f'Installed {name} version differs from the Denial package set: '
                             f'expected {version}, found {actual or "not installed"}')
    return {name: installed[name] for name in expected}


def verify_compositor(rootfs, digest, guest):
    path = guest_resolve(rootfs, COMPOSITOR)
    if not path.is_file() or overlay_packages.sha(path) != digest:
        raise ValueError('Installed Denial compositor differs from the published build')
    guest(['env', 'LD_BIND_NOW=1', COMPOSITOR, '--version'])


def configure_apt(rootfs, put):
    put(rootfs, APT_KEY.lstrip('/'), KEY.read_text())
    put(rootfs, 'etc/apt/' + APT_SOURCE,
        f'Types: deb\nURIs: https://{REPOSITORY}/apt\n'
        f'Suites: {APTORIGIN[0]}\nComponents: main\nArchitectures: arm64\n'
        f'Signed-By: {APT_KEY}\n')
    put(rootfs, 'etc/apt/preferences.d/denial',
        'Package: denial denial-flutter-engine denial-plugin-manager\n'
        f'Pin: origin {REPOSITORY}\nPin-Priority: 500\n\n'
        f'Package: *\nPin: origin {REPOSITORY}\nPin-Priority: -1\n')


def configure_pacman(rootfs, put):
    put(rootfs, 'etc/pacman.d/denial.asc', KEY.read_text())
    put(rootfs, 'etc/pacman.d/denial.conf',
        f'[denial]\nSigLevel = Required TrustedOnly\nServer = https://{REPOSITORY}/arch/$arch\n')
    config = guest_resolve(rootfs, '/etc/pacman.conf').read_text()
    if PACMAN_INCLUDE not in config.splitlines():
        put(rootfs, 'etc/pacman.conf', config.rstrip() + '\n\n' + PACMAN_INCLUDE + '\n')


def configure_session(rootfs, put):
    session = guest_resolve(rootfs, '/' + SESSION)
    if not session.is_file() or 'Exec=/usr/bin/denial-session' not in session.read_text().splitlines():
        raise ValueError('The Denial package did not install its Wayland session')
    # Use the launcher's native machine configuration and SDDM's packaged
    # Wayland session. Neither needs an extra service or a second login.
    config = 'etc/denial/session.conf'
    settings = guest_resolve(rootfs, '/' + config).read_text().splitlines()
    # The DPU scans out the panel, but the Adreno render node owns the scene GPU.
    # Use its persistent udev path; card and render-node numbers can change.
    defaults = ('DENIAL_SHELL_PROFILE=mobile',
                'DENIAL_RENDER_DEVICE=/dev/dri/by-path/platform-3d00000.gpu-render')
    missing = [setting for setting in defaults if setting not in settings]
    if missing:
        put(rootfs, config, '\n'.join([*settings, '', *missing]) + '\n')
    put(rootfs, 'etc/sddm.conf.d/10-sunuefi.conf',
        '[General]\nDisplayServer=wayland\n\n'
        '[Autologin]\nUser=piano\nSession=denial.desktop\n')
    put(rootfs, 'etc/xdg/autostart/org.fcitx.Fcitx5.desktop',
        '[Desktop Entry]\nName=Fcitx 5\nType=Application\n'
        'Exec=/usr/bin/fcitx5\nOnlyShowIn=Denial;\n')
    # Start the greeter after the native KMS device is available.
    name = 'sddm.service.d/20-native-kms.conf'
    put(rootfs, 'etc/systemd/system/' + name,
        (ROOT / 'linux/desktops/kde' / name).read_text())


def install_published_debian(rootfs, run, apt, manifest, install):
    from build_release_rootfs import target

    packages, engine, binary = published_set(manifest, 'debian-denial', debian_versions(rootfs))
    tmp = target(rootfs, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-denial-', dir=tmp) as folder:
        work = Path(folder)
        work.chmod(0o755)
        project, upstream = work / 'sunuefi', work / 'denial'
        project.mkdir()
        upstream.mkdir()
        paths = download_debian(rootfs, project, packages, run)
        paths += download_debian(
            rootfs, upstream, {engine['file']: {
                'name': ENGINE, 'version': engine['version'], 'architecture': 'arm64',
                'sha256': engine['sha256'],
            }}, run, source=APT_SOURCE)
        run([*apt, *install, '--reinstall', *paths])
    verify_compositor(rootfs, binary, lambda args: run(['chroot', rootfs, *args]))
    return {**{row['name']: row['version'] for row in packages.values()}, ENGINE: engine['version']}


def install_debian(rootfs, run, *, manifest=None):
    from build_release_rootfs import build_devices, build_resolver, put

    configure_apt(rootfs, put)
    apt = ['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get']
    install = ('install', '-y', '--no-remove', '--no-install-recommends')
    extras = (*DESKTOP_PACKAGES, 'fcitx5-frontend-gtk3', 'fcitx5-frontend-gtk4',
              'fcitx5-frontend-qt6', 'pipewire-pulse', 'qt6-virtualkeyboard-plugin',
              'xdg-desktop-portal', 'xdg-desktop-portal-gtk', 'xdg-desktop-portal-wlr')
    expected = None
    with build_devices(rootfs), build_resolver(rootfs):
        run([*apt, '-o', 'APT::Update::Error-Mode=any',
             '-o', 'Dir::Etc::sourcelist=' + APT_SOURCE, '-o', 'Dir::Etc::sourceparts=-',
             '-o', 'APT::Get::List-Cleanup=false', 'update'])
        if manifest is None:
            run([*apt, *install, *PACKAGES, *extras])
        else:
            expected = install_published_debian(rootfs, run, apt, manifest, install)
            run([*apt, *install, '--no-upgrade', *extras])
    versions = debian_versions(rootfs)
    versions = (verified_versions(expected, versions) if expected else
                {name: versions[name] for name in PACKAGES})
    configure_session(rootfs, put)
    run(['chroot', rootfs, 'systemctl', 'enable', 'sddm.service'])
    return versions


def install_published_arch(rootfs, guest, manifest):
    from build_release_rootfs import target

    installed = guest(['pacman', '-Qq'], text=True, stdout=subprocess.PIPE).stdout.splitlines()
    packages, _, binary = published_set(manifest, 'arch-denial', installed)
    tmp = target(rootfs, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-denial-', dir=tmp) as folder:
        work = Path(folder)
        work.chmod(0o755)
        overlay_packages.install_pacman(rootfs, work, 'sunuefi', packages, guest)
    verify_compositor(rootfs, binary, guest)
    return {row['name']: row['version'] for row in packages.values()}


def install_arch(rootfs, *, manifest=None):
    from build_nyarch_apps import guest_environment
    from build_release_rootfs import build_resolver, put

    configure_pacman(rootfs, put)
    extras = (*DESKTOP_PACKAGES, 'fcitx5-gtk', 'fcitx5-qt', 'pipewire-pulse',
              'qt6-virtualkeyboard', 'vulkan-freedreno')
    expected = None
    with build_resolver(rootfs), guest_environment(rootfs) as guest:
        guest(['pacman-key', '--add', '/etc/pacman.d/denial.asc'])
        guest(['pacman-key', '--lsign-key', FINGERPRINT])
        # Do not refresh the already selected Arch and SunUEFI indexes: a
        # newer library there may not match the installed hardware packages.
        sync_config = '/run/sunuefi-denial-sync.conf'
        put(rootfs, sync_config.lstrip('/'),
            '[options]\nArchitecture = aarch64\n\n' + PACMAN_INCLUDE + '\n')
        guest(['pacman', '--config', sync_config, '-Sy'])
        if manifest is None:
            targets = [f'denial/{package}' for package in PACKAGES]
        else:
            expected = install_published_arch(rootfs, guest, manifest)
            targets = []
        guest(['pacman', '-S', '--needed', '--noconfirm', *targets, *extras])
        result = guest(['pacman', '-Q', *(expected or PACKAGES)], text=True, stdout=subprocess.PIPE)
        versions = dict(line.split() for line in result.stdout.splitlines())
        if expected:
            versions = verified_versions(expected, versions)
        configure_session(rootfs, put)
        guest(['systemctl', 'enable', 'sddm.service'])
    return versions
