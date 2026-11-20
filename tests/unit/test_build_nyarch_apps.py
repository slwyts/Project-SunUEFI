"""Exercise Nyarch package versioning with makepkg's actual pkgver() pass."""
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import build_nyarch_apps as nyarch
from build import overlay_release
from package.archive import pacman_identity


@unittest.skipUnless(all(shutil.which(tool) for tool in ('makepkg', 'fakeroot', 'bsdtar')),
                     'makepkg, fakeroot and bsdtar are required')
class NyarchApplicationTests(unittest.TestCase):
    def test_published_pkgrel_survives_native_pkgver_update(self):
        with tempfile.TemporaryDirectory(prefix='sunuefi-nyarch-pkgver-') as temporary:
            root = Path(temporary)
            recipe = root / 'PKGBUILD'
            recipe.write_text(
                "pkgname=piano-nyarch-test\npkgver=1\npkgrel=1\narch=(any)\n"
                "pkgdesc='Package version test'\nlicense=(MIT)\noptions=(!debug !strip)\n"
                "pkgver() { printf '2.r7\\n'; }\n"
                "package() { install -Dm644 /dev/null \"$pkgdir/usr/share/nyarch-test\"; }\n")
            user = 'nobody' if os.geteuid() == 0 else None
            if user:
                account = pwd.getpwnam(user)
                for path in (root, recipe):
                    os.chown(path, account.pw_uid, account.pw_gid)
            commands = []

            def guest(args):
                commands.append(args[-1])
                # Only replace the chroot's ordinary build user on the host.
                self.assertEqual(args[:4], ['runuser', '-u', 'sunuefi-build', '--'])
                command = list(args)
                if user:
                    command[2] = user
                else:
                    command = command[4:]
                subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)

            with patch.dict(os.environ, {overlay_release.ENVIRONMENT: '44201533881.2',
                                         'PKGEXT': '.pkg.tar.gz',
                                         'BUILDDIR': str(root), 'PKGDEST': str(root),
                                         'SRCDEST': str(root)}):
                nyarch.make_application(guest, str(root), recipe)
                self.assertEqual(commands, ['--nobuild', '--noextract'])
                archives = list(root.glob('*.pkg.tar.gz'))
                self.assertEqual(len(archives), 1)
                name, version, arch = pacman_identity(archives[0])
                self.assertEqual((name, version, arch),
                                 ('piano-nyarch-test', '2.r7-' + overlay_release.pacman_release('1'), 'any'))
                self.assertTrue(overlay_release.uses_pacman_generation(version))
