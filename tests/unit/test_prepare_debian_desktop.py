"""Check signed-archive sid recipe selection without installing packages."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_debian_desktop as desktop
from desktop.power_migration import RETIRED_OVERLAY


class PrepareDebianDesktopTests(unittest.TestCase):
    def test_release_kde_replaces_only_generated_qt_scaling_with_output_scaling(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sessions = root / 'usr/share/wayland-sessions'
            sessions.mkdir(parents=True)
            (sessions / 'plasma.desktop').write_text('[Desktop Entry]\n')
            generated = []
            for home in ('etc/skel', 'home/piano'):
                scale = root / home / '.config/plasma-workspace/env/sunuefi-scale.sh'
                scale.parent.mkdir(parents=True)
                scale.write_text('export QT_SCALE_FACTOR="${QT_SCALE_FACTOR:-2}"\n')
                generated.append(scale)
            custom = generated[1].with_name('user-scale.sh')
            custom.write_text('export QT_SCALE_FACTOR=1.5\n')
            desktop.configure(root, Mock())
            self.assertTrue(all(not path.exists() for path in generated))
            self.assertEqual(custom.read_text(), 'export QT_SCALE_FACTOR=1.5\n')
            self.assertIn('Session=plasma.desktop', (root / 'etc/sddm.conf.d/10-sunuefi.conf').read_text())
            self.assertTrue((root / 'home/piano/.config/kwinoutputconfig.json').is_file())

    def public_recipe(self, folder):
        public = folder / 'public'
        (public / 'scripts').mkdir(parents=True)
        (public / 'rootfs').mkdir()
        (public / 'rootfs/packages-backports.txt').write_text('gnome-snapshot\npipewire-audio\n')
        (public / 'scripts/build-rootfs.sh').write_text('''#!/bin/bash
set -euo pipefail
SUITE=trixie OUTDIR='' KEYS='' USERSPACE_DIR='' MESA_DIR=''
die() { exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        --suite) SUITE=${2:?}; shift 2 ;;
        --mesa-dir) MESA_DIR=$(realpath "${2:?}"); shift 2 ;;
        -h|--help) echo 'Usage: build-rootfs.sh --suite trixie [--mesa-dir DIR]'; exit 0 ;;
        *) die ;;
    esac
done
[ "$SUITE" = trixie ] || die 'this desktop profile supports trixie only'
cat <<EOF
deb https://deb.debian.org/debian trixie main
deb https://deb.debian.org/debian trixie-updates main
deb https://security.debian.org/debian-security trixie-security main
EOF
printf 'deb http://deb.debian.org/debian trixie main\\n'
# Newer userspace from trixie-backports, with its dependencies.
echo trixie-backports
if [ -n "$USERSPACE_DIR" ]; then
    echo local-userspace
fi
if [ -n "$MESA_DIR" ]; then
    echo obsolete-mesa-pin
fi
# The checkout belongs to the building user; the rootfs must stay root-owned.
chroot "$ROOTFS" systemctl enable NetworkManager ssh bluetooth piano-usb piano-touch piano-radio
ln -sf /usr/lib/systemd/system/gdm3.service "$ROOTFS/etc/systemd/system/display-manager.service"
# Preserve the bootloader display: neither suspend nor blanking is recoverable yet.
chroot "$ROOTFS" systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
''')
        return public

    def test_kde_uses_its_native_input_method_with_xwayland_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = root / 'usr/share/wayland-sessions/plasma.desktop'
            session.parent.mkdir(parents=True)
            session.touch()
            autostart = root / 'etc/xdg/autostart/piano-fcitx5.desktop'
            autostart.parent.mkdir(parents=True)
            autostart.touch()

            desktop.configure(root, Mock())

            kwin = (root / 'home/piano/.config/kwinrc').read_text().splitlines()
            self.assertIn('InputMethod=/usr/share/applications/org.fcitx.Fcitx5.desktop', kwin)
            self.assertFalse(autostart.exists())
            settings = (root / 'etc/environment.d/60-piano-input-method.conf').read_text().splitlines()
            self.assertCountEqual(settings, [
                'QT_IM_MODULES=wayland;fcitx', 'QT_IM_MODULE=fcitx', 'XMODIFIERS=@im=fcitx',
            ])

    def test_kde_selects_its_packages_without_copying_packaged_profiles(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            public = self.public_recipe(folder)
            packages = ['chromium', 'pipewire-audio', 'gnome-shell', 'tuned', 'tuned-ppd']
            (public / 'rootfs/packages.txt').write_text('\n'.join(packages) + '\n')
            relative = 'usr/lib/tuned/profiles/piano-balanced/tuned.conf'
            profile = public / 'rootfs/overlay' / relative
            profile.parent.mkdir(parents=True)
            profile.write_text('upstream configuration\n')

            recipe = desktop.prepare(public, folder / 'derived', 'kde')
            selected = (recipe / 'rootfs/packages.txt').read_text().splitlines()
            self.assertNotIn('gnome-shell', selected)
            self.assertTrue({'tuned', 'tuned-ppd'}.issubset(selected))
            self.assertIn('powerdevil', selected)
            self.assertIn('pipewire-audio', selected)
            self.assertIn('fcitx5-frontend-gtk4', selected)
            self.assertIn('xdg-desktop-portal-gtk', selected)
            self.assertFalse((recipe / 'rootfs/overlay' / relative).exists())
            gnome = desktop.prepare(public, folder / 'gnome-derived', 'gnome')
            self.assertFalse((gnome / 'rootfs/overlay' / relative).exists())
            self.assertEqual((public / 'rootfs/packages.txt').read_text().splitlines(), packages)
            self.assertEqual(profile.read_text(), 'upstream configuration\n')

    def test_both_desktops_derive_sid_without_a_stable_abi_or_mesa_pin(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            public = self.public_recipe(folder)
            original = (public / 'scripts/build-rootfs.sh').read_bytes()
            (public / 'rootfs/packages.txt').write_text(
                'gnome-shell\npipewire-audio\nlibavcodec61\nfirefox-esr\nqbootctl\n')
            for name in ('gnome', 'kde'):
                recipe = desktop.prepare(public, folder / name, name)
                script = recipe / 'scripts/build-rootfs.sh'
                text = script.read_text()
                self.assertNotIn('trixie', text)
                self.assertNotIn('MESA_DIR', text)
                self.assertNotIn('obsolete-mesa-pin', text)
                self.assertIn('deb https://deb.debian.org/debian sid main contrib', text)
                self.assertIn('deb-src https://deb.debian.org/debian sid main contrib', text)
                subprocess.run(['bash', '-n', script], check=True)
                self.assertNotEqual(subprocess.run(
                    ['bash', script, '--suite', 'trixie'],
                    capture_output=True).returncode, 0)
                selected = (recipe / 'rootfs/packages.txt').read_text().splitlines()
                self.assertTrue({'chromium', 'ffmpeg', 'mesa-vulkan-drivers',
                                 'gnome-snapshot'}.issubset(selected))
                self.assertTrue({'libavcodec61', 'qbootctl', 'firefox-esr'}.isdisjoint(selected))
                self.assertEqual(selected.count('pipewire-audio'), 1)
                self.assertFalse((recipe / 'rootfs/packages-backports.txt').exists())
            self.assertEqual((public / 'scripts/build-rootfs.sh').read_bytes(), original)

    def test_both_recipes_leave_packaged_files_and_service_activation_to_the_package(self):
        public = ROOT / 'upstream/debian-piano-current'
        upstream = public / 'rootfs/overlay'
        if not upstream.is_dir():
            self.skipTest('Pinned Debian source not fetched')
        packaged = desktop.packaged_overlay_paths()
        excluded = packaged | RETIRED_OVERLAY
        upstream_paths = {name for name in packaged
                          if (upstream / name).is_file() or (upstream / name).is_symlink()}
        self.assertTrue(upstream_paths)
        self.assertTrue(any((upstream / path).is_symlink() for path in upstream_paths))
        with tempfile.TemporaryDirectory() as folder:
            for name in ('gnome', 'kde'):
                recipe = desktop.prepare(public, Path(folder) / name, name)
                for relative in excluded:
                    path = recipe / 'rootfs/overlay' / relative
                    self.assertFalse(path.exists() or path.is_symlink(), relative)
                script = recipe / 'scripts/build-rootfs.sh'
                enabled, = [line.split('systemctl enable ', 1)[1].split()
                            for line in script.read_text().splitlines() if 'systemctl enable ' in line]
                self.assertTrue({'piano-usb', 'piano-touch', 'piano-camera', 'piano-camerad'}.isdisjoint(enabled))
                self.assertTrue({'NetworkManager', 'bluetooth'}.issubset(enabled))
                masked, = [line.split('systemctl mask ', 1)[1].split()
                           for line in script.read_text().splitlines() if 'systemctl mask ' in line]
                self.assertCountEqual(masked, ('hibernate.target', 'hybrid-sleep.target',
                                              'suspend-then-hibernate.target'))
                self.assertFalse((recipe / 'rootfs/overlay/usr/lib/systemd/system/piano-camerad.service').exists())
                subprocess.run(['bash', '-n', script], check=True)
        self.assertTrue(all((upstream / path).is_file() or (upstream / path).is_symlink()
                            for path in upstream_paths))

    def test_source_drift_rejects_mixed_suite_output(self):
        with self.assertRaisesRegex(ValueError, 'APT setup changed'):
            desktop.sid_recipe('#!/bin/sh\nexit 0\n')


if __name__ == '__main__':
    unittest.main()
