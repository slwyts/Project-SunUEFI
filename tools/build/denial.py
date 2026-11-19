"""Install Denial's signed native packages and configure the tablet session."""
from pathlib import Path
import subprocess

from build_piano_ram_bootstrap import guest_resolve


ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = 'packages.denialwm.org'
KEY = ROOT / 'config/keys/denial-repo-key.asc'
FINGERPRINT = 'AE4108FA5E91E26BE0EE331E0F5B3AD16E023091'
APTORIGIN = ('trixie', 'trixie', 'Denial')
PACKAGES = ('denial', 'denial-flutter-engine')
DESKTOP_PACKAGES = ('fcitx5', 'fcitx5-chinese-addons', 'foot', 'sddm', 'thunar', 'upower',
                    'weston', 'wireplumber')
APT_SOURCE = 'sources.list.d/denial.sources'
APT_KEY = '/etc/apt/keyrings/denial.asc'
PACMAN_INCLUDE = 'Include = /etc/pacman.d/denial.conf'
SESSION = 'usr/share/wayland-sessions/denial.desktop'


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
    profile = 'DENIAL_SHELL_PROFILE=mobile'
    if profile not in settings:
        put(rootfs, config, '\n'.join([*settings, '', profile]) + '\n')
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


def install_debian(rootfs, run):
    from build_release_rootfs import build_devices, build_resolver, put

    configure_apt(rootfs, put)
    apt = ['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get']
    with build_devices(rootfs), build_resolver(rootfs):
        run([*apt, '-o', 'APT::Update::Error-Mode=any',
             '-o', 'Dir::Etc::sourcelist=' + APT_SOURCE, '-o', 'Dir::Etc::sourceparts=-',
             '-o', 'APT::Get::List-Cleanup=false', 'update'])
        run([*apt, 'install', '-y', '--no-remove', '--no-install-recommends',
             *PACKAGES, *DESKTOP_PACKAGES, 'fcitx5-frontend-gtk3', 'fcitx5-frontend-gtk4',
             'fcitx5-frontend-qt6', 'pipewire-pulse', 'qt6-virtualkeyboard-plugin',
             'xdg-desktop-portal', 'xdg-desktop-portal-gtk', 'xdg-desktop-portal-wlr'])
    configure_session(rootfs, put)
    run(['chroot', rootfs, 'systemctl', 'enable', 'sddm.service'])
    versions = subprocess.check_output(
        ['chroot', rootfs, 'dpkg-query', '-W', '-f=${Package} ${Version}\n', *PACKAGES], text=True)
    return dict(line.split() for line in versions.splitlines())


def install_arch(rootfs):
    from build_nyarch_apps import guest_environment
    from build_release_rootfs import build_resolver, put

    configure_pacman(rootfs, put)
    with build_resolver(rootfs), guest_environment(rootfs) as guest:
        guest(['pacman-key', '--add', '/etc/pacman.d/denial.asc'])
        guest(['pacman-key', '--lsign-key', FINGERPRINT])
        # Do not refresh the already selected Arch and SunUEFI indexes: a
        # newer library there may not match the installed hardware packages.
        sync_config = '/run/sunuefi-denial-sync.conf'
        put(rootfs, sync_config.lstrip('/'),
            '[options]\nArchitecture = aarch64\n\n' + PACMAN_INCLUDE + '\n')
        guest(['pacman', '--config', sync_config, '-Sy'])
        guest(['pacman', '-S', '--needed', '--noconfirm',
               *(f'denial/{package}' for package in PACKAGES), *DESKTOP_PACKAGES,
               'fcitx5-gtk', 'fcitx5-qt', 'pipewire-pulse', 'qt6-virtualkeyboard',
               'vulkan-freedreno'])
        configure_session(rootfs, put)
        guest(['systemctl', 'enable', 'sddm.service'])
        result = guest(['pacman', '-Q', *PACKAGES], text=True, stdout=subprocess.PIPE)
    return dict(line.split() for line in result.stdout.splitlines())
