#!/usr/bin/env python3
"""Apply the selected Nyarch desktop assets and shared Piano defaults."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from build_nyarch_apps import guest_environment
from build_piano_runtime_helpers import source_patches
from stage_piano_full_userspace import stage_gnome_power

ROOT = Path(__file__).resolve().parents[1]


def stage(root, source, extensions=None):
    root, source = root.resolve(), source.resolve()
    if not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use a project-built rootfs')
    commit = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True):
        raise ValueError('Nyarch asset source must be clean')
    origin = source / 'Gnome/etc/skel/.local/share'
    inputs = {}
    for folder in ('themes', 'icons', 'backgrounds'):
        src = origin / folder
        dst = root / 'usr/share' / folder
        shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
        for path in src.rglob('*'):
            if path.is_file() and not path.is_symlink():
                inputs[str(path.relative_to(source))] = hashlib.sha256(path.read_bytes()).hexdigest()
    license_file = root / 'usr/share/licenses/nyarch-desktop/LICENSE'
    license_file.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / 'LICENSE', license_file)
    # Preserve tablet defaults; no x86-only Node addons or forced incompatible
    # extensions are imported from the upstream desktop skeleton.
    stage_gnome_power(root)
    dconf = root / 'etc/dconf/db/local.d/10-nyarch'
    dconf.write_text(
        "[org/gnome/desktop/interface]\n"
        "gtk-theme='adw-gtk3'\nicon-theme='Tela-circle-MaterialYou'\n"
        "color-scheme='prefer-dark'\nenable-animations=true\n\n"
        "[org/gnome/desktop/input-sources]\n"
        "sources=[('xkb', 'us'), ('ibus', 'libpinyin')]\n\n"
        "[org/gnome/desktop/background]\npicture-options='zoom'\n"
        "picture-uri='file:///usr/share/backgrounds/Nyarch_Wallpaper.png'\n"
        "picture-uri-dark='file:///usr/share/backgrounds/Nyarch_Wallpaper.png'\n\n"
        "[org/gnome/shell]\nfavorite-apps=['org.gnome.Nautilus.desktop', "
        "'chromium.desktop', 'org.gnome.Console.desktop', 'moe.nyarchlinux.customize.desktop']\n")
    if extensions:
        enabled = []
        defaults = {
            'user-theme@gnome-shell-extensions.gcampax.github.com',
            'material-you-colors@francescocaracciolo.github.io',
            'blur-my-shell@aunetx', 'dash-to-dock@micxgx.gmail.com',
            'gsconnect@andyholmes.github.io', 'arch-update@RaphaelRochet',
        }
        for folder in sorted(extensions.iterdir()):
            if not folder.is_dir() or not (folder / 'metadata.json').is_file():
                continue
            metadata = json.loads((folder / 'metadata.json').read_text())
            if '51' not in metadata.get('shell-version', []):
                raise ValueError('Extension has no GNOME 51 support: ' + folder.name)
            shutil.copytree(folder, root / 'usr/share/gnome-shell/extensions' / folder.name,
                            symlinks=True, dirs_exist_ok=True)
            if metadata['uuid'] == 'blur-my-shell@aunetx':
                patch, = source_patches(ROOT / 'patches/blur-my-shell/75/series.json')
                subprocess.run([
                    'patch', '--batch', '--fuzz=0', '-p1', '-i',
                    str(patch),
                ], cwd=root / 'usr/share/gnome-shell/extensions' / folder.name, check=True)
            if metadata['uuid'] == 'gsconnect@andyholmes.github.io':
                config = root / 'usr/share/gnome-shell/extensions' / folder.name / 'config.js'
                text = config.read_text().replace('/usr/local/share/gnome-shell/extensions',
                                                 '/usr/share/gnome-shell/extensions')
                text = text.replace('/usr/local/share/glib-2.0/schemas',
                                    '/usr/share/gnome-shell/extensions/' + folder.name + '/schemas')
                text = text.replace('/usr/local/share/locale',
                                    '/usr/share/gnome-shell/extensions/' + folder.name + '/locale')
                text = text.replace('/usr/local/lib/x86_64-linux-gnu', '/usr/lib')
                config.write_text(text)
            if metadata['uuid'] in defaults:
                enabled.append(metadata['uuid'])
        if 'blur-my-shell@aunetx' not in enabled:
            raise ValueError('Nyarch desktop requires Blur my Shell')
        with dconf.open('a') as stream:
            stream.write('\nenabled-extensions=' + repr(enabled) +
                         '\ndisable-user-extensions=false\n'
                         '\n[org/gnome/shell/extensions/user-theme]\nname=\'MaterialYou\'\n'
                         '\n[org/gnome/shell/extensions/blur-my-shell/panel]\nforce-light-text=true\n'
                         '\n[org/gnome/shell/extensions/blur-my-shell/popup]\nstatic-blur=true\n'
                         '\n[org/gnome/shell/extensions/blur-my-shell/overview]\nstyle-components=0\n'
                         '\n[org/gnome/shell/extensions/dash-to-dock]\n'
                         'dock-position=\'BOTTOM\'\ndash-max-icon-size=48\n'
                         'height-fraction=0.9\n')
            if 'material-you-colors@francescocaracciolo.github.io' in enabled:
                stream.write(
                    '\n[org/gnome/shell/extensions/material-you-colors]\n'
                    'python-backend=false\nenable-pywal-theming=false\n'
                    'arcmenu-theming=false\nshow-notifications=false\n')
    # Keep ID=arch so pacman-based desktop tools choose the right backend.
    release = root / 'usr/lib/os-release'
    text = release.read_text()
    rows = [line for line in text.splitlines() if not line.startswith(
        ('NAME=', 'PRETTY_NAME=', 'VARIANT=', 'VARIANT_ID='))]
    release.write_text('\n'.join(rows) + '\nNAME="Nyarch Linux"\n'
                       'PRETTY_NAME="Nyarch Linux (SunUEFI ARM64)"\n'
                       'VARIANT="Piano GNOME"\nVARIANT_ID=sunuefi\n')
    (root / 'etc/locale.gen').write_text('en_US.UTF-8 UTF-8\nzh_CN.UTF-8 UTF-8\n')
    (root / 'etc/locale.conf').write_text(
        'LANG=zh_CN.UTF-8\nLANGUAGE=zh_CN:zh\n')
    locale_environment = root / 'etc/environment.d/60-piano-locale.conf'
    locale_environment.parent.mkdir(parents=True, exist_ok=True)
    locale_environment.write_text('LANGUAGE=zh_CN:zh\n')
    input_environment = root / 'etc/environment.d/60-piano-input-method.conf'
    input_environment.parent.mkdir(parents=True, exist_ok=True)
    input_environment.write_text(
        'GTK_IM_MODULE=ibus\nQT_IM_MODULE=ibus\n'
        'XMODIFIERS=@im=ibus\nSDL_IM_MODULE=ibus\n')
    (root / 'etc/xdg/autostart/piano-ibus.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=IBus\n'
        'Exec=ibus-daemon --replace --xim\nOnlyShowIn=GNOME;\n'
        'NoDisplay=true\n')
    (root / 'etc/hostname').write_text('piano\n')
    (root / 'etc/gdm/custom.conf').write_text(
        '[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=piano\n')
    shutil.copytree(root / 'usr/share/themes/MaterialYou',
                    root / 'home/piano/.local/share/themes/MaterialYou',
                    symlinks=True, dirs_exist_ok=True)
    with guest_environment(root) as guest:
        guest(['chown', '-R', 'piano:piano', '/home/piano/.local/share/themes'])
        guest(['locale-gen'])
        guest(['dconf', 'update'])
        guest(['glib-compile-schemas', '/usr/share/glib-2.0/schemas'])
        for folder in (root / 'usr/share/gnome-shell/extensions').glob('*/schemas'):
            guest(['glib-compile-schemas', '/' + str(folder.relative_to(root))])
        guest(['gtk-update-icon-cache', '-f', '-t', '/usr/share/icons/Tela-circle-MaterialYou'])
    record = {'source_url': 'https://github.com/NyarchLinux/NyarchLinux',
              'source_commit': commit, 'source_files': inputs,
              'desktop': 'gnome', 'base': 'Arch Linux ARM rolling',
              'extension_policy': 'No unsupported x86 binary addons or disabled version checks'}
    path = root / 'usr/share/piano-provenance/nyarch-desktop.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--extensions', type=Path)
    args = parser.parse_args()
    # Caller uses unshare; mount lifetime is limited to guest_environment.
    stage(args.rootfs, args.source, args.extensions)
