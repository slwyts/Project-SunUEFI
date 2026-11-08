#!/usr/bin/env python3
"""Derive the selected desktop's base recipe without altering the upstream tree."""
from pathlib import Path
import shutil

GNOME_PACKAGES = {
    'gdm3', 'gnome-control-center', 'gnome-session', 'gnome-settings-daemon',
    'gnome-shell', 'gnome-system-monitor', 'gnome-terminal', 'gnome-text-editor',
    'nautilus', 'evolution-data-server', 'ibus', 'ibus-libpinyin',
    'task-chinese-s-gnome-desktop',
}
KDE_PACKAGES = [
    'task-kde-desktop', 'plasma-workspace', 'sddm', 'dolphin', 'konsole',
    'systemsettings', 'powerdevil', 'plasma-nm', 'plasma-pa', 'kscreen',
    'task-chinese-s-kde-desktop',
    'fonts-noto', 'poppler-data', 'xdg-desktop-portal-kde',
    'fcitx5', 'fcitx5-chinese-addons',
    'fcitx5-frontend-gtk3', 'fcitx5-frontend-qt6', 'kde-config-fcitx5',
    'fcitx5-frontend-qt5', 'maliit-keyboard',
]
GNOME_PATHS = (
    'etc/gdm3', 'etc/dconf', 'usr/share/glib-2.0/schemas/90_piano.gschema.override',
    'usr/lib/systemd/user/piano-power-button.service', 'usr/lib/piano/power-button',
    # KDE uses powerdevil, so the tuned profiles and their GNOME mapping go away
    # with the rest of the GNOME-only overlay.
    'usr/lib/tuned/profiles/piano-power-saver',
    'usr/lib/tuned/profiles/piano-balanced',
    'usr/lib/tuned/profiles/piano-performance',
    'etc/tuned/piano-ppd.conf',
    'usr/lib/systemd/system/tuned-ppd.service.d',
    'etc/systemd/user/graphical-session.target.wants/piano-power-button.service',
    'etc/systemd/logind.conf.d/piano.conf', 'etc/udev/rules.d/70-piano-power-button.rules',
)


def gnome_path(name):
    return any(name == prefix or name.startswith(prefix + '/') for prefix in GNOME_PATHS)


def prepare(public, output, desktop):
    if desktop == 'gnome':
        return public
    if desktop != 'kde':
        raise ValueError('Unsupported Debian desktop')
    destination = output / 'base-recipe'
    shutil.copytree(public / 'scripts', destination / 'scripts')
    shutil.copytree(public / 'rootfs', destination / 'rootfs', symlinks=True)
    for name in GNOME_PATHS:
        path = destination / 'rootfs/overlay' / name
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
    package_file = destination / 'rootfs/packages.txt'
    packages = [line for line in package_file.read_text().splitlines()
                if line.strip() not in GNOME_PACKAGES | {'firefox-esr'}]
    package_file.write_text('\n'.join(packages + KDE_PACKAGES + ['chromium']) + '\n')
    script = destination / 'scripts/build-rootfs.sh'
    text = script.read_text()
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
    # Use desktop settings; do not ship GNOME's exclusive evdev power helper.
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
    elif input_method != 'ibus':
        raise ValueError('Unsupported input method: ' + input_method)
    input_environment = rootfs / 'etc/environment.d/60-piano-input-method.conf'
    input_environment.parent.mkdir(parents=True, exist_ok=True)
    input_environment.write_text(
        (f'GTK_IM_MODULE={input_method}\nQT_IM_MODULE={input_method}\n'
         if input_method == 'fcitx' else '') +
        f'XMODIFIERS=@im={input_method}\nSDL_IM_MODULE={input_method}\n')
    # Panel source modes require fitting even when listed as driver modes.
    # KWin otherwise explicitly requests None for every non-generated mode.
    (input_environment.parent / '60-piano-display.conf').write_text(
        'KWIN_DRM_FORCE_SCALING_MODE=FULL\n')
    if input_method == 'fcitx':
        autostart = rootfs / 'etc/xdg/autostart/piano-fcitx5.desktop'
        autostart.parent.mkdir(parents=True, exist_ok=True)
        autostart.write_text(
            '[Desktop Entry]\nType=Application\nName=Fcitx 5\n'
            'Exec=fcitx5 -d --replace\nOnlyShowIn=KDE;\nNoDisplay=true\n')
    overlay = Path(__file__).resolve().parents[1] / 'linux/desktops/kde/config'
    for source in sorted(overlay.iterdir()):
        shutil.copyfile(source, config / source.name)
    # KWin chooses screen scaling from the panel dimensions. An application
    # QT_SCALE_FACTOR would double-scale Qt and leave GTK applications behind.
    run(['chroot', rootfs, 'chown', '-R', 'piano:piano', '/home/piano/.config'], check=True)
