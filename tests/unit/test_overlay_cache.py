"""Check distribution package reuse against actual archives and repo configuration."""
from contextlib import nullcontext
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_nyarch_rootfs as nyarch
from build import overlay_cache, overlay_packages


class OverlayCacheTests(unittest.TestCase):
    def test_recipe_keys_ignore_host_checks_and_keep_package_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', '-C', root, 'init', '-q'], check=True)
            not_inputs = ('tools/check_host.py', 'tools/prepare_nv_runtime_guard.py')
            builder = 'tools/build/overlay_packages.py'
            for name in (*not_inputs, builder):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('original\n')
            subprocess.run(['git', '-C', root, 'add', '.'], check=True)
            keys = (overlay_cache.recipe_identity, overlay_cache.debian_recipe_identity)
            original = [key(root) for key in keys]
            for name in not_inputs:
                (root / name).write_text('edited\n')
                self.assertEqual([key(root) for key in keys], original, name)
            (root / builder).write_text('edited\n')
            for key, previous in zip(keys, original):
                self.assertNotEqual(key(root), previous)

    @unittest.skipUnless(shutil.which('pacman-conf'), 'pacman-conf required')
    def test_uses_valve_database_path_and_detects_index_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sync = root / 'usr/lib/holo/pacmandb/sync'
            sync.mkdir(parents=True)
            config = root / 'pacman.conf'
            config.write_text(f'[options]\nDBPath = {sync.parent}\n'
                              'Architecture = aarch64\nSigLevel = Required\n'
                              '[core]\nServer = https://example.invalid/$repo/os/$arch\n')
            (sync / 'core.db').write_bytes(b'old index')
            capture = subprocess.check_output

            def run(command, *, text):
                self.assertEqual(command[:5], ['chroot', root, 'env', 'LC_ALL=C', 'pacman-conf'])
                # The real parser resolves DBPath before returning it.
                result = capture(['pacman-conf', '--config', config, *command[5:]], text=text)
                return result.replace(str(root), '')

            with patch.object(overlay_cache.subprocess, 'check_output', side_effect=run):
                before = overlay_cache.pacman_state(root)
                self.assertEqual(before['databases']['core.db'], overlay_packages.sha(sync / 'core.db'))
                (sync / 'core.db').write_bytes(b'new index')
                self.assertNotEqual(overlay_cache.pacman_state(root), before)
                (sync / 'core.db.sig').write_bytes(b'new signature')
                self.assertIn('core.db.sig', overlay_cache.pacman_state(root)['databases'])

    @unittest.skipUnless(shutil.which('bsdtar'), 'libarchive tools required')
    def test_reuses_exact_native_archives_and_rejects_stale_inputs_and_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = 'nyarch-gnome'
            base = root / 'arch-root.tar.gz'
            base.write_bytes(b'Arch Linux ARM base fixture')
            inputs = {'builder_image': 'test-image', 'recipes': 'test-recipe',
                      'base': overlay_packages.sha(base), 'kernel': {'source_tree': 'test-tree'},
                      'upstream': {'nyarch': 'test-git'}, 'repositories': {'core.db': 'before'},
                      'installed': 'test-package-state'}
            names = ('libfprint', 'fprintd', 'ffmpeg', 'libwacom', 'libssc',
                     'iio-sensor-proxy', 'fastrpc', 'pipewire-v4l2-source',
                     'gst-plugin-pipewire', 'mesa', 'gdm', 'mutter', 'gnome-settings-daemon')
            packages = {}
            for name in names:
                archive = root / (name + '-1-1-aarch64.pkg.tar.gz')
                info = f'pkgname = {name}\npkgver = 1-1\narch = aarch64\n'.encode()
                with tarfile.open(archive, 'w:gz') as stream:
                    member = tarfile.TarInfo('.PKGINFO')
                    member.size = len(info)
                    stream.addfile(member, io.BytesIO(info))
                self.assertEqual(overlay_packages.pacman_identity(archive), (name, '1-1', 'aarch64'))
                packages[archive.name] = (archive, {
                    'name': name, 'version': '1-1', 'architecture': 'aarch64',
                    'sha256': overlay_packages.sha(archive), 'component': name,
                })
            prior = root / 'build/overlay-cache' / target
            expected = overlay_packages.export(prior, target, 'sunuefi', packages, {}, build_inputs=inputs)
            out = root / 'output'
            for field in inputs:
                with self.subTest(field=field):
                    self.assertIsNone(overlay_cache.restore_pacman(root, out, target, {
                        **inputs, field: 'changed',
                    }))
                    self.assertFalse(out.exists())

            config = root / 'config'
            config.mkdir()
            (config / 'nyarch.json').write_text(json.dumps({'base_url': base.as_uri()}))
            (config / 'release.json').write_text('{"sensors": {}}')
            source = root / 'build/release/source-manifest.json'
            source.parent.mkdir()
            source.write_text(json.dumps({'worktree': str(root / 'linux')}))
            (root / 'build/nyarch-inputs/sensors').mkdir(parents=True)
            downloaded = root / 'build/nyarch-inputs/ArchLinuxARM-aarch64.tar.gz'
            self.assertFalse(downloaded.exists())
            out = root / 'artifacts/packages/overlays' / target
            args = SimpleNamespace(output=root / 'build/distros' / (target + '-piano'),
                                   packages_only=out, resume=False)

            def assemble(_):
                directory = root / 'build/distros/overlays' / target / 'applications'
                directory.mkdir(parents=True)
                (directory / 'SOURCE.json').write_text('{}')

            with patch.object(nyarch, 'ROOT', root), \
                    patch.object(nyarch.os, 'geteuid', return_value=0), \
                    patch.object(nyarch.assemble_rootfs, 'plan'), \
                    patch.object(nyarch.assemble_rootfs, 'execute', side_effect=assemble), \
                    patch.object(nyarch.release, 'build_resolver', return_value=nullcontext()), \
                    patch.object(nyarch.build_nyarch_apps, 'guest_environment',
                                 return_value=nullcontext(lambda _: None)), \
                    patch.object(overlay_cache, 'pacman_inputs', return_value=inputs) as identity:
                self.assertEqual(nyarch.build(args), expected)
                self.assertEqual(identity.call_args.args[3], inputs['base'])
            self.assertEqual(downloaded.read_bytes(), base.read_bytes())
            self.assertEqual(json.loads((out / 'manifest.json').read_text()), expected)
            for name, (_, row) in packages.items():
                self.assertEqual(overlay_packages.sha(out / name), row['sha256'])
            (prior / archive.name).write_bytes(b'corrupted archive')
            missing = root / 'not-published'
            self.assertIsNone(overlay_cache.restore_pacman(root, missing, target, inputs))
            self.assertFalse(missing.exists())


if __name__ == '__main__':
    unittest.main()
