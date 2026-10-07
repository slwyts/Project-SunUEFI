"""Host fixtures for distro/desktop dispatch; no base download or package install."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('assemble_fixture', ROOT / 'tools/assemble_rootfs.py')
tool = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(tool)


class AssembleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(ROOT / 'linux/rootfs', self.root / 'linux/rootfs')
        shutil.copytree(ROOT / 'linux/desktops', self.root / 'linux/desktops')

    def args(self, distro='debian', desktop='gnome', *extra):
        return tool.parser().parse_args(['--distro', distro, '--desktop', desktop, '--plan', *extra])

    def package(self, name, target, component='config', ready=True, extension='.deb'):
        folder = self.root / name; folder.mkdir()
        filename = name + extension; data = ('fixture ' + name).encode(); (folder / filename).write_bytes(data)
        manifest = {'schema_version': 1, 'component': component, 'package_ready': ready, 'target': target,
                    'files': {filename: {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'format': 'deb'}}}
        (folder / 'manifest.json').write_text(json.dumps(manifest)); return folder

    def test_plan_is_readonly_and_reports_recipe_not_real_boot(self):
        with patch.object(tool.subprocess, 'run', side_effect=AssertionError('plan executed command')):
            result = tool.plan(self.args(), self.root)
        self.assertEqual(result['status'], 'HOST_RECIPE_CANDIDATE'); self.assertFalse(result['device_tested'])
        self.assertEqual(result['storage']['root_partname'], 'sunuefi_root')
        self.assertEqual(result['storage']['root_label'], 'PIANOROOT')
        self.assertFalse(Path(result['output']).exists())

    def test_ubuntu_is_verified_noble24_profile_not_invented26(self):
        result = tool.plan(self.args('ubuntu'), self.root)
        self.assertEqual(result['target'], {'distro': 'ubuntu', 'suite': 'noble', 'arch': 'aarch64'})
        self.assertEqual(result['profile']['name'], 'Ubuntu 24.04 LTS')
        self.assertIn('ubuntu-archive-keyring.gpg', result['keyring'])
        with self.assertRaisesRegex(ValueError, 'Suite differs'):
            tool.plan(self.args('ubuntu', 'gnome', '--suite', 'unreviewed'), self.root)

    def test_arch_requires_real_tarball_digest_and_has_no_fallback(self):
        result = tool.plan(self.args('arch', 'kde'), self.root)
        self.assertEqual(result['status'], 'INPUTS_REQUIRED'); self.assertIn('base_sha256', result['missing_inputs'])
        with patch.object(tool.subprocess, 'run', side_effect=AssertionError('missing hash downloaded')):
            with self.assertRaisesRegex(ValueError, 'Required explicit'):
                tool.execute(result)
        self.assertFalse(Path(result['output']).exists())

    def test_deepin_needs_eight_explicit_inputs_and_never_guesses_base(self):
        result = tool.plan(self.args('deepin', 'dde'), self.root)
        self.assertEqual(result['status'], 'INPUTS_REQUIRED'); self.assertEqual(len(result['missing_inputs']), 8)
        self.assertIsNone(result['base_url'])
        with patch.object(tool.subprocess, 'run', side_effect=AssertionError('unknown base downloaded')):
            with self.assertRaises(ValueError): tool.execute(result)

    def test_unavailable_matrix_is_clear(self):
        for distro, desktop in (('debian', 'dde'), ('ubuntu', 'dde'), ('arch', 'dde'), ('deepin', 'gnome')):
            with self.subTest(distro=distro):
                result = tool.plan(self.args(distro, desktop), self.root)
                self.assertEqual(result['status'], 'UNAVAILABLE')
                self.assertFalse(Path(result['output']).exists())

    def test_output_overwrite_or_escape_is_rejected(self):
        output = self.root / 'build/distros/old'; output.mkdir(parents=True); (output / 'keep').write_text('old')
        for path in (output, self.root / 'outside'):
            with self.assertRaisesRegex(ValueError, 'new output'):
                tool.plan(self.args('debian', 'gnome', '--output', str(path)), self.root)
        self.assertEqual((output / 'keep').read_text(), 'old')

    def test_package_target_and_bytes_are_checked(self):
        target = {'distro': 'debian', 'suite': 'trixie', 'arch': 'aarch64'}
        directory = self.package('config', target)
        self.assertEqual(tool.bundle(directory, target)['component'], 'config')
        with self.assertRaisesRegex(ValueError, 'target distro'):
            tool.bundle(directory, {**target, 'distro': 'ubuntu', 'suite': 'noble'})
        (directory / 'config.deb').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'identity changed'): tool.bundle(directory, target)

    def test_arch_rejects_debian_package_even_with_forged_target_label(self):
        target = {'distro': 'arch', 'suite': 'rolling', 'arch': 'aarch64'}
        directory = self.package('config', target)
        with self.assertRaisesRegex(ValueError, 'Package format'): tool.bundle(directory, target)

    def test_recipe_only_bsp_and_missing_compiled_layers_are_not_complete(self):
        target = {'distro': 'debian', 'suite': 'trixie', 'arch': 'aarch64'}
        recipe = self.package('recipe', target, ready=False)
        with self.assertRaisesRegex(ValueError, 'not a built'): tool.bundle(recipe, target)
        bsp = self.package('config', target)
        result = tool.plan(self.args('debian', 'gnome', '--bsp', str(bsp)), self.root)
        self.assertEqual(set(result['missing_inputs']), {'mesa', 'runtime', 'modules', 'firmware'})
        self.assertEqual(result['status'], 'INPUTS_REQUIRED')

    def test_complete_target_layers_use_bsp_manifest_fields(self):
        target = {'distro': 'debian', 'suite': 'trixie', 'arch': 'aarch64'}
        bsp = self.package('config', target); extra = ['--bsp', str(bsp)]
        for component in sorted(tool.LAYERS): extra += ['--layer', str(self.package(component, target, component))]
        result = tool.plan(self.args('debian', 'gnome', *extra), self.root)
        self.assertEqual(result['status'], 'HOST_RECIPE_CANDIDATE')
        self.assertEqual({row['component'] for row in result['layers']}, tool.LAYERS)

    def test_deepin_cannot_mix_debian_ubuntu_sources(self):
        rows = tool.deepin_sources('deb https://community-packages.deepin.com/deepin/beige beige main\n', 'beige')
        self.assertIn('signed-by=/usr/share/keyrings/sunuefi-archive.gpg', rows[0])
        for text in ('deb https://deb.debian.org/debian trixie main',
                     'deb https://ports.ubuntu.com/ubuntu-ports noble main',
                     'deb https://community-packages.deepin.com/deepin/beige wrong main'):
            with self.assertRaisesRegex(ValueError, 'no Debian/Ubuntu mixing'): tool.deepin_sources(text, 'beige')

    def test_package_availability_requires_a_real_candidate(self):
        self.assertFalse(tool.apt_available('')); self.assertFalse(tool.apt_available('Candidate: (none)\n'))
        self.assertTrue(tool.apt_available('  Candidate: 1.2-3\n'))

    def test_tar_parent_symlink_escape_is_rejected_before_extract(self):
        archive = self.root / 'bad.tar'
        with tarfile.open(archive, 'w') as tar:
            link = tarfile.TarInfo('etc'); link.type = tarfile.SYMTYPE; link.linkname = '/old-root'; tar.addfile(link)
            child = tarfile.TarInfo('etc/fstab'); child.size = 1; tar.addfile(child, io.BytesIO(b'x'))
        with self.assertRaisesRegex(ValueError, 'symlink-parent'): tool.validate_archive(archive)

    def test_desktop_scope_does_not_invent_keyboard_or_rotation(self):
        catalog = json.loads((self.root / 'linux/desktops/catalog.json').read_text())['desktops']
        self.assertIn('built-in', catalog['gnome']['osk']); self.assertNotIn('caribou', catalog['gnome']['apt_packages'])
        self.assertIn('only when', catalog['kde']['osk']); self.assertIn('unverified', catalog['dde']['osk'])
        self.assertIn('unverified', catalog['gnome']['rotation'])


if __name__ == '__main__': unittest.main()
