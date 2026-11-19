#!/usr/bin/env python3
"""Derive the Debian sid desktop recipe without altering the upstream tree."""
from pathlib import Path
import copy
import json
import shutil

from package_bsp import payload, power_profiles_payload
from desktop.power_migration import RETIRED_OVERLAY

GNOME_PACKAGES = {
    'gdm3', 'gnome', 'task-gnome-desktop', 'gnome-control-center',
    'gnome-session', 'gnome-settings-daemon',
    'gnome-shell', 'gnome-system-monitor', 'gnome-terminal', 'gnome-text-editor',
    'nautilus', 'evolution-data-server', 'ibus', 'ibus-libpinyin',
    'ibus-gtk', 'ibus-gtk3', 'ibus-gtk4',
    'task-chinese-s-gnome-desktop',
}
KDE_PACKAGES = [
    'task-kde-desktop', 'plasma-desktop', 'plasma-workspace', 'sddm', 'dolphin', 'konsole',
    'systemsettings', 'powerdevil', 'upower', 'plasma-nm', 'plasma-pa', 'bluedevil', 'kscreen',
    'task-chinese-s-kde-desktop',
    'fonts-noto', 'poppler-data', 'xdg-desktop-portal-kde', 'xdg-desktop-portal-gtk',
    'fcitx5', 'fcitx5-chinese-addons',
    'fcitx5-frontend-gtk3', 'fcitx5-frontend-gtk4', 'fcitx5-frontend-qt5',
    'fcitx5-frontend-qt6', 'kde-config-fcitx5', 'maliit-keyboard',
    'libpam-kwallet5', 'systemd-coredump',
]
GNOME_PATHS = (
    'etc/gdm3', 'etc/dconf', 'usr/share/glib-2.0/schemas/90_piano.gschema.override',
)


def gnome_path(name):
    return any(name == prefix or name.startswith(prefix + '/') for prefix in GNOME_PATHS)


def packaged_overlay_paths():
    """Paths owned by native packages rather than the public overlay."""
    _, rows = payload(['camera'])
    return {row['path'] for row, _ in (*rows, *power_profiles_payload())}


def sid_recipe(text):
    """Replace the fixed source recipe's stable-only APT setup as one operation."""
    replacements = (
        ('[ "$SUITE" = trixie ] || die \'this desktop profile supports trixie only\'',
         '[ "$SUITE" = sid ] || die \'this desktop profile supports sid only\''),
        ('deb https://deb.debian.org/debian trixie main\n'
         'deb https://deb.debian.org/debian trixie-updates main\n'
         'deb https://security.debian.org/debian-security trixie-security main',
         'deb https://deb.debian.org/debian sid main contrib\n'
         'deb-src https://deb.debian.org/debian sid main contrib'),
        ("printf 'deb http://deb.debian.org/debian trixie main\\n'",
         "printf 'deb http://deb.debian.org/debian sid main\\n'"),
    )
    for old, replacement in replacements:
        if text.count(old) != 1:
            raise ValueError('Upstream Debian APT setup changed')
        text = text.replace(old, replacement)
    start = text.index('# Newer userspace from trixie-backports,')
    end = text.index('if [ -n "$USERSPACE_DIR" ]; then', start)
    text = text[:start] + text[end:]
    start = text.index('if [ -n "$MESA_DIR" ]; then')
    end = text.index('# The checkout belongs to the building user;', start)
    text = text[:start] + text[end:]
    text = text.replace('chroot "$ROOTFS" apt-get update',
                        'chroot "$ROOTFS" apt-get -o APT::Update::Error-Mode=any update')
    text = text.replace(' -o Dir::Etc::sourceparts=- update',
                        ' -o Dir::Etc::sourceparts=- -o APT::Update::Error-Mode=any update')
    text = text.replace('SUITE=trixie ', 'SUITE=sid ')
    text = text.replace('--suite trixie', '--suite sid')
    text = text.replace(" USERSPACE_DIR='' MESA_DIR=''", " USERSPACE_DIR=''")
    text = text.replace(' [--mesa-dir DIR]', '')
    text = text.replace('        --mesa-dir) MESA_DIR=$(realpath "${2:?}"); shift 2 ;;\n', '')
    # The device package uses udev to activate USB and touch on matching hardware.
    enable = 'chroot "$ROOTFS" systemctl enable NetworkManager ssh bluetooth'
    upstream_enable = enable + ' piano-usb piano-touch '
    if text.count(upstream_enable) != 1:
        raise ValueError('Upstream Debian service activation changed')
    text = ''.join(line.replace(upstream_enable, enable + ' ')
                   .replace(' piano-camera piano-camerad ', ' ')
                   if line.startswith(upstream_enable) else line
                   for line in text.splitlines(keepends=True))
    # Allow memory suspend; keep the disk hibernation modes masked.
    mask = 'chroot "$ROOTFS" systemctl mask '
    hibernation = 'hibernate.target hybrid-sleep.target suspend-then-hibernate.target'
    upstream_mask = ('# Preserve the bootloader display: neither suspend nor blanking is recoverable yet.\n'
                     + mask + 'sleep.target suspend.target ' + hibernation)
    if text.count(upstream_mask) != 1:
        raise ValueError('Upstream Debian sleep policy changed')
    text = text.replace(upstream_mask, mask + hibernation)
    if 'trixie' in text or 'MESA_DIR' in text:
        raise ValueError('Stable-only Debian source remains in the derived sid recipe')
    return text


def prepare(public, output, desktop):
    if desktop not in ('gnome', 'kde'):
        raise ValueError('Unsupported Debian desktop')
    destination = output / 'base-recipe'
    (destination / 'scripts').mkdir(parents=True)
    shutil.copy2(public / 'scripts/build-rootfs.sh', destination / 'scripts/build-rootfs.sh')
    source_rootfs = public / 'rootfs'
    excluded_paths = {Path('overlay') / path for path in packaged_overlay_paths() | RETIRED_OVERLAY}

    def ignore_overlay(directory, names):
        relative = Path(directory).relative_to(source_rootfs)
        return {name for name in names if relative / name in excluded_paths}

    shutil.copytree(source_rootfs, destination / 'rootfs', symlinks=True, ignore=ignore_overlay)
    if desktop == 'kde':
        for name in GNOME_PATHS:
            path = destination / 'rootfs/overlay' / name
            if path.is_symlink() or path.is_file():
                path.unlink()
            elif path.is_dir():
                shutil.rmtree(path)
    package_file = destination / 'rootfs/packages.txt'
    packages = [line.strip() for line in package_file.read_text().splitlines()
                if line.strip() and not line.lstrip().startswith('#')]
    backports = destination / 'rootfs/packages-backports.txt'
    packages.extend(line.strip() for line in backports.read_text().splitlines()
                    if line.strip() and not line.lstrip().startswith('#'))
    backports.unlink()
    # Codec SONAME dependencies come from FFmpeg, not the old stable ABI list.
    obsolete = {'firefox-esr', 'libavcodec61', 'qbootctl'}
    if desktop == 'kde':
        obsolete |= GNOME_PACKAGES
        packages.extend(KDE_PACKAGES)
    packages = list(dict.fromkeys([p for p in packages if p not in obsolete] +
                                  ['chromium', 'ffmpeg', 'mesa-vulkan-drivers']))
    package_file.write_text('\n'.join(packages) + '\n')
    script = destination / 'scripts/build-rootfs.sh'
    text = sid_recipe(script.read_text())
    if desktop == 'kde':
        old = 'ln -sf /usr/lib/systemd/system/gdm3.service "$ROOTFS/etc/systemd/system/display-manager.service"'
        if text.count(old) != 1:
            raise ValueError('Upstream display-manager selection changed')
        text = text.replace(old, 'ln -sf /usr/lib/systemd/system/sddm.service "$ROOTFS/etc/systemd/system/display-manager.service"')
        text = text.replace('desktop=gnome', 'desktop=kde').replace('GNOME autologin', 'Plasma autologin')
    script.write_text(text)
    return destination


def configure(rootfs, run, input_method='fcitx'):
    sessions = sorted((rootfs / 'usr/share/wayland-sessions').glob('*plasma*.desktop'))
    if len(sessions) != 1:
        raise ValueError('Expected one installed Plasma Wayland session')
    path = rootfs / 'etc/sddm.conf.d/10-sunuefi.conf'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('[Autologin]\nUser=piano\nSession=' + sessions[0].name + '\n')
    desktop = Path(__file__).resolve().parents[1] / 'linux/desktops/kde'
    dropin = Path('sddm.service.d/20-native-kms.conf')
    destination = rootfs / 'etc/systemd/system' / dropin
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(desktop / dropin, destination)
    # The generic stock KDE base uses per-application Qt scaling. The release
    # uses compositor output scaling, so remove only that generated default.
    for home in ('etc/skel', 'home/piano'):
        scale = rootfs / home / '.config/plasma-workspace/env/sunuefi-scale.sh'
        if scale.is_file() and scale.read_text() == 'export QT_SCALE_FACTOR="${QT_SCALE_FACTOR:-2}"\n':
            scale.unlink()
    config = rootfs / 'home/piano/.config'
    config.mkdir(parents=True, exist_ok=True)
    (config / 'kdeglobals').write_text('[KDE]\nAnimationDurationFactor=1\n'
                                     '[Locale]\nLanguage=zh_CN:zh\n')
    (config / 'plasma-localerc').write_text(
        '[Formats]\nLANG=zh_CN.UTF-8\n\n'
        '[Translations]\nLANGUAGE=zh_CN:zh\n')
    if input_method == 'fcitx':
        (config / 'fcitx5/profile').parent.mkdir(parents=True, exist_ok=True)
        (config / 'fcitx5/profile').write_text(
            '[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=pinyin\n\n'
            '[Groups/0/Items/0]\nName=keyboard-us\nLayout=\n\n'
            '[Groups/0/Items/1]\nName=pinyin\nLayout=\n\n'
            '[GroupOrder]\n0=Default\n')
        # KWin launches the input method with its privileged Wayland socket.
        # An ordinary autostart client cannot bind input-method-v1.
        (config / 'kwinrc').write_text(
            '[Wayland]\nInputMethod=/usr/share/applications/org.fcitx.Fcitx5.desktop\n'
            'VirtualKeyboardEnabled=true\n')
    elif input_method != 'ibus':
        raise ValueError('Unsupported input method: ' + input_method)
    input_environment = rootfs / 'etc/environment.d/60-piano-input-method.conf'
    input_environment.parent.mkdir(parents=True, exist_ok=True)
    settings = f'XMODIFIERS=@im={input_method}\n'
    if input_method == 'fcitx':
        # Plasma's GTK settings portal fallback lets GTK pick its own module.
        # Qt prefers Wayland and retains the Fcitx plugin for X11 and Qt 5.
        settings += 'QT_IM_MODULES=wayland;fcitx\nQT_IM_MODULE=fcitx\n'
    else:
        settings += 'SDL_IM_MODULE=ibus\n'
    input_environment.write_text(settings)
    # Panel source modes require fitting even when listed as driver modes.
    # KWin otherwise explicitly requests None for every non-generated mode.
    (input_environment.parent / '60-piano-display.conf').write_text(
        'KWIN_DRM_FORCE_SCALING_MODE=FULL\n')
    if input_method == 'fcitx':
        autostart = rootfs / 'etc/xdg/autostart/piano-fcitx5.desktop'
        autostart.unlink(missing_ok=True)
    for source in sorted((desktop / 'config').iterdir()):
        if source.name != 'kwinoutputconfig.json':
            shutil.copyfile(source, config / source.name)
            continue
        output_config = json.loads(source.read_text())
        outputs = next(row['data'] for row in output_config if row['name'] == 'outputs')
        setups = next(row['data'] for row in output_config if row['name'] == 'setups')
        # Match the synthesized fixed-panel identity as well as old EDID-less
        # kernels. Only the selected display exists, so setup indices differ.
        output = copy.deepcopy(outputs[0])
        output['edidIdentifier'] = 'XMI 129 0 0 2025 0'
        outputs.append(output)
        setup = copy.deepcopy(setups[0])
        setup['outputs'][0]['outputIndex'] = len(outputs) - 1
        setups.append(setup)
        (config / source.name).write_text(json.dumps(output_config, indent=2) + '\n')
    # KWin chooses screen scaling from the panel dimensions. An application
    # QT_SCALE_FACTOR would double-scale Qt and leave GTK applications behind.
    run(['chroot', rootfs, 'chown', '-R', 'piano:piano', '/home/piano/.config'], check=True)
