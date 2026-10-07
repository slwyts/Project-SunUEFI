"""Exercise real local Git sources and disposable firmware preparation."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from firmware_workspace import active_root, prepare, validate_sources


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), '-c', 'commit.gpgsign=false',
                                    '-c', 'protocol.file.allow=always', *args], stderr=subprocess.PIPE)


def repository(path, name):
    path.mkdir(parents=True)
    git(path, 'init', '-q')
    git(path, 'config', 'user.name', 'Workspace Test')
    git(path, 'config', 'user.email', 'workspace@example.invalid')
    (path / 'source.c').write_text(name + '\n')
    git(path, 'add', 'source.c')
    git(path, 'commit', '-qm', 'test: initial source')
    return git(path, 'rev-parse', 'HEAD').decode().strip()


class FirmwareWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='firmware-workspace-')
        self.base = Path(self.tmp.name)
        self.root = self.base / 'project'
        repository(self.root, 'project')
        pins = {}
        for name, child in (('Mu-Silicium', 'Mu_Basecore'), ('simple-init', 'libs/freetype')):
            source = self.root / 'upstream' / name
            repository(source, name)
            nested = self.base / (name + '-nested')
            nested_pin = repository(nested, child)
            git(source, 'submodule', 'add', '-q', str(nested), child)
            git(source, 'commit', '-qam', 'test: fixed nested source')
            pins[name] = {'commit': git(source, 'rev-parse', 'HEAD').decode().strip()}
            pins[name + '/' + child] = {'commit': nested_pin}
        for relative, text in {
            'tools/build_product.sh': '#!/bin/sh\necho product\n',
            'tools/unrelated_linux.py': '# unrelated Linux tool\n',
            'uefi/core/Core.c': 'int core = 1;\n',
            'uefi/core/Old.h': '#define OLD 1\n',
            'uefi/platforms/pianoProbePkg/pianoProbe.dsc': '[Defines]\n',
            'vendor/piano/manifest.json': '{}\n',
            'config/piano-product.json': '{}\n',
            'patches/firmware/series.json': '{}\n',
            'docs/devel/building.md': 'host build docs\n',
            'uefi/README.md': 'source documentation\n',
        }.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        shutil.copyfile(ROOT / 'tools/firmware_workspace.py', self.root / 'tools/firmware_workspace.py')
        (self.root / 'sources.lock.json').write_text(json.dumps(pins))
        (self.root / '.gitignore').write_text('upstream/\nbuild/\nartifacts/\n.venv/\nuefi/platforms/pianoProductPkg/\n')
        git(self.root, 'add', '.')
        git(self.root, 'commit', '-qm', 'test: firmware inputs')
        for directory in ('.venv', 'build/host-tools', 'upstream/linux-piano', 'upstream/dtc'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        self.tmp.cleanup()

    def test_sources_are_copied_and_original_dirty_changes_are_preserved(self):
        mu = self.root / 'upstream/Mu-Silicium'
        (mu / 'source.c').write_text('original local edits\n')
        (mu / 'scratch.txt').write_text('keep this untracked file\n')
        before = git(mu, 'status', '--porcelain')
        generated = self.root / 'uefi/platforms/pianoProductPkg/generated.c'
        generated.parent.mkdir(parents=True)
        generated.write_text('old generated product\n')
        (self.root / 'uefi/core/untracked.c').write_text('not a registered input\n')
        workspace = prepare(self.root)
        self.assertEqual(git(mu, 'status', '--porcelain'), before)
        self.assertEqual((mu / 'source.c').read_text(), 'original local edits\n')
        self.assertEqual((workspace / 'upstream/Mu-Silicium/source.c').read_text(), 'Mu-Silicium\n')
        self.assertFalse((workspace / 'upstream/Mu-Silicium/scratch.txt').exists())
        for relative in ('tools/build_product.sh', 'uefi/core/Core.c', 'vendor/piano/manifest.json',
                         'config/piano-product.json', 'patches/firmware/series.json'):
            self.assertEqual((workspace / relative).read_bytes(), (self.root / relative).read_bytes())
            self.assertFalse((workspace / relative).is_symlink())
        self.assertFalse((workspace / 'uefi/platforms/pianoProductPkg').exists())
        self.assertEqual((workspace / 'uefi/core/untracked.c').read_bytes(),
                         (self.root / 'uefi/core/untracked.c').read_bytes())
        self.assertFalse((workspace / 'docs').exists())

    def test_nested_commit_and_patch_are_isolated_in_workspace(self):
        workspace = prepare(self.root)
        original = self.root / 'upstream/Mu-Silicium/Mu_Basecore'
        copy = workspace / 'upstream/Mu-Silicium/Mu_Basecore'
        self.assertEqual(git(original, 'rev-parse', 'HEAD'), git(copy, 'rev-parse', 'HEAD'))
        patch = self.base / 'nested.patch'
        patch.write_text('diff --git a/source.c b/source.c\n--- a/source.c\n+++ b/source.c\n'
                         '@@ -1 +1 @@\n-Mu_Basecore\n+patched build source\n')
        git(copy, 'apply', str(patch))
        self.assertEqual((copy / 'source.c').read_text(), 'patched build source\n')
        self.assertEqual((original / 'source.c').read_text(), 'Mu_Basecore\n')
        self.assertEqual(git(original, 'status', '--porcelain'), b'')
        self.assertTrue(validate_sources(workspace))

    def test_repeated_prepare_syncs_canonical_and_restarts_patch_sources(self):
        workspace = prepare(self.root)
        copy = workspace / 'upstream/simple-init/libs/freetype'
        (copy / 'source.c').write_text('previous overlay\n')
        (copy / 'extra.c').write_text('previous copied overlay\n')
        (self.root / 'uefi/core/Core.c').write_text('int core = 2;\n')
        git(self.root, 'rm', '-q', 'uefi/core/Old.h')
        with self.assertRaises(ValueError):
            validate_sources(workspace, ['uefi/core/Core.c'])
        self.assertEqual(prepare(self.root), workspace)
        self.assertEqual((workspace / 'uefi/core/Core.c').read_text(), 'int core = 2;\n')
        self.assertFalse((workspace / 'uefi/core/Old.h').exists())
        self.assertEqual((copy / 'source.c').read_text(), 'libs/freetype\n')
        self.assertFalse((copy / 'extra.c').exists())
        self.assertTrue(validate_sources(self.root))

    def test_repeated_prepare_fetches_an_upgraded_local_upstream_commit(self):
        workspace = prepare(self.root)
        original = self.root / 'upstream/Mu-Silicium'
        (original / 'source.c').write_text('new upstream source\n')
        git(original, 'commit', '-qam', 'test: upstream update')
        commit = git(original, 'rev-parse', 'HEAD').decode().strip()
        pinfile = self.root / 'sources.lock.json'
        pins = json.loads(pinfile.read_text())
        pins['Mu-Silicium']['commit'] = commit
        pinfile.write_text(json.dumps(pins))
        self.assertEqual(prepare(self.root), workspace)
        self.assertEqual(git(workspace / 'upstream/Mu-Silicium', 'rev-parse', 'HEAD').decode().strip(), commit)
        self.assertTrue(validate_sources(self.root))

    def test_actual_build_inputs_ignore_unrelated_tools_and_documentation(self):
        workspace = prepare(self.root)
        (self.root / 'tools/unrelated_linux.py').write_text('# changed unrelated tool\n')
        (self.root / 'uefi/README.md').write_text('updated documentation\n')
        self.assertTrue(validate_sources(self.root, ['uefi/core/Core.c']))
        self.assertTrue(validate_sources(workspace, ['uefi/core/Core.c', 'uefi/README.md']))
        with self.assertRaises(ValueError):
            validate_sources(self.root)
        (self.root / 'uefi/core/Core.c').write_text('changed while building\n')
        with self.assertRaises(ValueError):
            validate_sources(workspace, ['uefi/core/Core.c'])

    def test_original_root_can_reach_outputs_and_workspace_selection(self):
        self.assertEqual(active_root(self.root), self.root)
        workspace = prepare(self.root)
        self.assertEqual(active_root(self.root), workspace)
        self.assertEqual(active_root(workspace), workspace)
        output = workspace / 'artifacts/product/PianoUEFI-product.img'
        output.parent.mkdir(parents=True)
        output.write_bytes(b'built image')
        self.assertEqual((self.root / 'artifacts/product/PianoUEFI-product.img').read_bytes(), b'built image')
        self.assertEqual((workspace / '.venv').resolve(), self.root / '.venv')
        self.assertEqual((workspace / 'build/host-tools').resolve(), self.root / 'build/host-tools')
        self.assertEqual((workspace / 'build/logs').resolve(), self.root / 'build/logs')

    def test_build_cli_executes_the_projected_entrypoint(self):
        entry = self.root / 'tools/build_product.sh'
        entry.write_text('#!/bin/bash\nset -eu\n'
                         'test -f "$(dirname "$0")/../.firmware-workspace.json"\n'
                         'printf "projected product build\\n"\n')
        output = subprocess.check_output([sys.executable, str(self.root / 'tools/firmware_workspace.py'), 'build'])
        self.assertEqual(output, b'projected product build\n')


if __name__ == '__main__':
    unittest.main()
