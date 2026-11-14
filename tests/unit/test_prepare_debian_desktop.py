"""Check signed-archive sid recipe selection without installing packages."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_debian_desktop as desktop


class PrepareDebianDesktopTests(unittest.TestCase):
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
ln -sf /usr/lib/systemd/system/gdm3.service "$ROOTFS/etc/systemd/system/display-manager.service"
''')
        return public

    def test_kde_filters_tuned_packages_with_their_configuration(self):
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
            self.assertTrue({'tuned', 'tuned-ppd', 'gnome-shell'}.isdisjoint(selected))
            self.assertIn('powerdevil', selected)
            self.assertIn('pipewire-audio', selected)
            self.assertFalse((recipe / 'rootfs/overlay' / relative).exists())
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

    def test_source_drift_rejects_mixed_suite_output(self):
        with self.assertRaisesRegex(ValueError, 'APT setup changed'):
            desktop.sid_recipe('#!/bin/sh\nexit 0\n')


if __name__ == '__main__':
    unittest.main()
