"""Check the derived KDE recipe without installing packages or touching a rootfs."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_debian_desktop as desktop


class PrepareDebianDesktopTests(unittest.TestCase):
    def test_kde_filters_tuned_packages_with_their_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            public = folder / 'public'
            (public / 'scripts').mkdir(parents=True)
            (public / 'rootfs').mkdir()
            packages = ['chromium', 'pipewire-audio', 'gnome-shell', 'tuned', 'tuned-ppd']
            (public / 'rootfs/packages.txt').write_text('\n'.join(packages) + '\n')
            command = 'ln -sf /usr/lib/systemd/system/gdm3.service "$ROOTFS/etc/systemd/system/display-manager.service"\n'
            (public / 'scripts/build-rootfs.sh').write_text(command)
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


if __name__ == '__main__':
    unittest.main()
