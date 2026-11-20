"""Actual Git change lists select public product builds, without device inputs."""
from fnmatch import fnmatchcase
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ci_build_targets as targets

PRODUCTS = ['debian-gnome', 'debian-kde', 'debian-denial', 'nyarch-gnome',
            'arch-denial', 'steamos']


class CiBuildTargetsTests(unittest.TestCase):
    def test_arch_images_share_two_abi_matched_provider_sets(self):
        for target in ('nyarch-gnome', 'steamos'):
            selected = targets.select([], target)
            self.assertEqual(selected, [target])
            self.assertEqual(targets.overlay_targets(selected), ['nyarch-gnome', 'steamos'])
        self.assertEqual(targets.overlay_targets(['debian-kde']), ['debian-gnome'])
        self.assertEqual(targets.overlay_targets(['linux']), [])

    def test_denial_overlays_do_not_select_unrelated_images(self):
        self.assertEqual(targets.overlay_targets(['debian-denial']), ['debian-gnome', 'debian-denial'])
        self.assertEqual(targets.overlay_targets(['arch-denial']), ['nyarch-gnome', 'steamos', 'arch-denial'])
        self.assertEqual(targets.select(['patches/denial/0.6.8/series.json']),
                         ['debian-denial', 'arch-denial'])
        self.assertEqual(targets.select(['containers/denial.Dockerfile']),
                         ['debian-denial', 'arch-denial'])
        self.assertEqual(targets.select(['config/denial.json', 'linux/dts/piano-display.dtso']), PRODUCTS)

    def test_domains_and_shared_inputs(self):
        self.assertEqual(targets.select(['tools/build/check_patch_series.py']), [])
        self.assertEqual(targets.select(['uefi/core/product/PianoProductCore.c']), PRODUCTS)
        self.assertEqual(targets.select(['linux/dts/piano-audio-dmic-clock.dtso']), PRODUCTS)
        self.assertEqual(targets.select(['config/release.json']), PRODUCTS)
        self.assertEqual(targets.select(['sources.lock.json']), PRODUCTS)
        self.assertEqual(targets.select(['tools/build_simpleinit.sh']), PRODUCTS)
        self.assertEqual(targets.select(['patches/firmware/series.json']), PRODUCTS)
        self.assertEqual(targets.select(['config/mesa.json']), PRODUCTS)
        self.assertEqual(targets.select(['patches/piano-sensors/import.patch']), PRODUCTS)
        self.assertEqual(targets.select(['android/native/piano-boot-request.c']), PRODUCTS)
        self.assertEqual(targets.select(['uefi/handoff/bootselect/BootRequest.h']), PRODUCTS)
        self.assertEqual(targets.select(['tools/export_installer.py']), PRODUCTS)
        self.assertEqual(targets.select(['docs/user/install-from-artifact.md']), PRODUCTS)
        self.assertEqual(targets.select(['uefi/core/product/PianoProductCore.c', 'linux/config']), PRODUCTS)
        self.assertEqual(targets.select(['README.md', 'docs/status.md']), [])
        self.assertEqual(targets.select(['linux/dts/piano-pen-charger.dtso']), PRODUCTS)
        self.assertEqual(targets.select([], 'debian-gnome'), ['debian-gnome'])
        self.assertEqual(targets.select(['linux/config'], 'uefi'), ['uefi'])
        self.assertEqual(targets.select(['config/release.json'], 'linux'), ['linux'])
        with self.assertRaises(ValueError):
            targets.select([], 'unknown')

    def test_real_git_diff_and_new_push_fallback(self):
        with tempfile.TemporaryDirectory(prefix='ci-targets-') as temporary:
            root = Path(temporary)
            env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], env=env, text=True).strip()
            def commit(message):
                git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                    '-c', 'commit.gpgsign=false', 'commit', '-qm', message)
                return git('rev-parse', 'HEAD')

            def assert_changes(base, *paths):
                files = targets.changed_files(root, base)
                self.assertCountEqual(filter(None, files), paths)
                self.assertEqual(targets.select(files), PRODUCTS)

            git('init', '-q')
            git('config', 'diff.renames', 'true')
            (root / 'linux').mkdir(); (root / 'linux/config').write_text('base\n')
            (root / 'build.sh').write_text('shared\n'); git('add', '.')
            before = commit('base')
            (root / 'linux/config').write_text('new\n'); git('add', '.')
            modified = commit('Linux change')
            assert_changes(before, 'linux/config')
            self.assertEqual(targets.select(targets.changed_files(root, '0' * 40)), PRODUCTS)

            (root / 'docs').mkdir()
            git('mv', 'linux/config', 'docs/config')
            moved_out = commit('move out of product inputs')
            assert_changes(modified, 'linux/config', 'docs/config')
            (root / 'linux').mkdir(exist_ok=True)
            git('mv', 'docs/config', 'linux/settings')
            moved_in = commit('move into product inputs')
            assert_changes(moved_out, 'docs/config', 'linux/settings')

            git('rm', 'linux/settings')
            commit('remove product input')
            assert_changes(moved_in, 'linux/settings')

    def test_workflow_path_filter_agrees_on_tracked_inputs(self):
        workflow = ROOT / '.github/workflows/build-products.yml'
        lines = workflow.read_text().splitlines()
        rules = []
        for line in lines[lines.index('    paths:') + 1:]:
            if not line.startswith('      - '):
                break
            rules.append(line.removeprefix('      - ').strip("'\""))

        def included(path):
            enabled = False
            for rule in rules:
                pattern = rule.removeprefix('!')
                if pattern.endswith('/**'):
                    matches = path.startswith(pattern.removesuffix('**'))
                else:
                    matches = path.count('/') == pattern.count('/') and fnmatchcase(path, pattern)
                if matches:
                    enabled = not rule.startswith('!')
            return enabled

        tracked = subprocess.check_output(['git', '-C', ROOT, 'ls-files', '-z'], text=True)
        tracked = [path for path in tracked.split('\0') if path]
        self.assertEqual([path for path in tracked if included(path)],
                         [path for path in tracked if targets.select([path])])

    def test_host_checks_changes_alone_do_not_select_products(self):
        check_files = ['.github/workflows/host-checks.yml', 'tools/check_host.py',
                       'tests/unit/test_camera.py', 'tests/native/PianoBootOptionsTest.c']
        self.assertEqual(targets.select(check_files), [])
        self.assertEqual(targets.select([*check_files, '.github/workflows/_rootfs.yml']), PRODUCTS)
        self.assertEqual(targets.select([*check_files, 'tools/package/camera.py']), PRODUCTS)

    def test_windows_media_does_not_publish_linux_images(self):
        files = ['tools/build/windows.py', 'windows/deployment/probe.cmd',
                 '.github/workflows/windows.yml']
        self.assertEqual(targets.select(files), [])
        self.assertEqual(targets.select([*files, 'uefi/components/acpi/Dsdt.asl']), PRODUCTS)


if __name__ == '__main__':
    unittest.main()
