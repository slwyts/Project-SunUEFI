"""Exercise build configuration and command isolation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_chroot import pacman_build_config


class PacmanBuildConfigTests(unittest.TestCase):
    def test_arch_build_mirror_preserves_installed_files_and_signed_sources(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'rootfs'
            (root / 'etc/pacman.d').mkdir(parents=True)
            config = root / 'etc/pacman.conf'
            mirrorlist = root / 'etc/pacman.d/mirrorlist'
            original = ('[options]\nCheckSpace\nSigLevel = Required DatabaseOptional\n'
                        '[core]\nInclude = /etc/pacman.d/mirrorlist\n'
                        '[extra]\n\tInclude=/etc/pacman.d/mirrorlist  # Arch Linux ARM\n'
                        'Include = /etc/pacman.d/sunuefi.conf\n'
                        'Include = /etc/pacman.d/denial.conf\n')
            mirrors = b'Server = http://mirror.archlinuxarm.org/$arch/$repo\n'
            config.write_text(original)
            mirrorlist.write_bytes(mirrors)
            mirror = 'https://arch.example/archlinuxarm'

            temporary = pacman_build_config(root, mirror=mirror)
            self.assertIn('SigLevel = Required DatabaseOptional\n', temporary)
            self.assertIn('Include = /etc/pacman.d/sunuefi.conf\n', temporary)
            self.assertIn('Include = /etc/pacman.d/denial.conf\n', temporary)
            self.assertEqual(temporary.count(f'Server = {mirror}/$arch/$repo\n'), 2)
            self.assertNotIn('/etc/pacman.d/mirrorlist', temporary)

            manifest = root.parent / 'manifest.json'
            record = {'distro': 'arch', 'pacman_build_mirror': mirror}
            manifest.write_text(json.dumps(record))
            self.assertEqual(pacman_build_config(root), temporary)
            manifest.write_text(json.dumps({**record, 'distro': 'steamos'}))
            self.assertEqual(pacman_build_config(root), original.replace(
                '\nCheckSpace\n', '\n# CheckSpace (build chroot)\n'))
            self.assertEqual(config.read_bytes(), original.encode())
            self.assertEqual(mirrorlist.read_bytes(), mirrors)

    def test_arch_build_mirror_rejects_http(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'etc').mkdir()
            (root / 'etc/pacman.conf').write_text('[core]\nInclude = /etc/pacman.d/mirrorlist\n')
            with self.assertRaisesRegex(ValueError, 'public HTTPS'):
                pacman_build_config(root, mirror='http://arch.example')


@unittest.skipUnless(os.geteuid() == 0 and shutil.which('cc') and shutil.which('unshare'),
                     'Needs root, a static C toolchain and Linux namespaces')
class BuildChrootTests(unittest.TestCase):
    def test_daemon_cannot_pin_dev_after_changing_its_root(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
            folder = Path(folder)
            root = folder / 'rootfs'
            for name in ('proc', 'dev', 'usr/bin'):
                (root / name).mkdir(parents=True, exist_ok=True)
            source = folder / 'daemon.c'
            source.write_text(textwrap.dedent(r'''
                #define _GNU_SOURCE
                #include <fcntl.h>
                #include <signal.h>
                #include <stdlib.h>
                #include <sys/stat.h>
                #include <sys/types.h>
                #include <unistd.h>

                int main(void)
                {
                    int ready[2];
                    char value;
                    if (pipe(ready)) return 1;
                    pid_t child = fork();
                    if (child < 0) return 2;
                    if (!child) {
                        close(ready[0]);
                        if (setsid() < 0 || chroot("/dev") || chdir("/")) _exit(3);
                        int fd = open("/retained", O_CREAT | O_WRONLY, 0600);
                        if (fd < 0) _exit(4);
                        signal(SIGTERM, SIG_IGN);
                        if (write(ready[1], "1", 1) != 1) _exit(5);
                        close(ready[1]);
                        for (;;) pause();
                    }
                    close(ready[1]);
                    return read(ready[0], &value, 1) == 1 ? 0 : 6;
                }
            '''))
            subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                            source, '-o', root / 'usr/bin/daemon'], check=True, timeout=30)
            worker = textwrap.dedent('''
                import subprocess
                import sys
                from pathlib import Path
                sys.path.insert(0, sys.argv[1])
                from build_chroot import guest_command, stop_helpers
                root = Path(sys.argv[2])
                def run(*args):
                    return subprocess.run(list(map(str, args)), check=True, timeout=10)
                run('mount', '-t', 'proc', 'proc', root / 'proc')
                run('mount', '-t', 'tmpfs', 'tmpfs', root / 'dev')
                run(*guest_command(root, ['/usr/bin/daemon']))
                assert (root / 'dev/retained').is_file()
                # The helper changed its root to /dev, so root-only scans
                # cannot find it. PID namespace teardown must have reaped it.
                stop_helpers(root)
                (root / 'dev/pts').mkdir()
                run('mount', '-t', 'tmpfs', 'tmpfs', root / 'dev/pts')
                run('umount', '--recursive', root / 'dev')
                run('umount', '--recursive', root / 'proc')
                assert not (root / 'dev/retained').exists()
            ''')
            subprocess.run(['unshare', '--mount', '--propagation', 'private',
                            sys.executable, '-c', worker, str(ROOT / 'tools'), str(root)],
                           check=True, timeout=40)


if __name__ == '__main__':
    unittest.main()
