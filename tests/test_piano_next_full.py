"""Canonical Next source/config/actual-module checks; no Image rebuild/device."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_next_full as next_full

CANDIDATE = ROOT / 'artifacts/kernels/next-full/498569101e34'


class PianoNextFullTests(unittest.TestCase):
    def test_actual_canonical_source_and_complete_config(self):
        proof = next_full.source_proof()
        self.assertEqual(proof['source_commit'], next_full.COMMIT)
        self.assertEqual(len(proof['canonical_device_patch_commits']), 17)
        self.assertEqual(len(proof['upstream_generic_patches_omitted']), 8)
        public = (next_full.WORK / 'arch/arm64/configs/piano_rootfs.config').read_text()
        command = next_full.full.command_line((ROOT / 'configs/linux/piano-full.config').read_text(), public, 'ram')
        config = (CANDIDATE / 'config').read_text()
        self.assertEqual(len(next_full.full.validate_config(config, public, command)), 50)
        for name in ('BATTERY_PIANO_MCA', 'CHARGER_SC8541', 'HID_NANOSIC_WN8030', 'VIDEO_QCOM_CAMSS'):
            with self.subTest(symbol=name), self.assertRaises(ValueError):
                next_full.full.validate_config(config.replace('CONFIG_' + name + '=m', '# CONFIG_' + name + ' is not set'), public, command)

    def test_actual_sealed_candidate_readonly_recheck(self):
        before = next_full.sha(CANDIDATE / 'manifest.json')
        result = next_full.check_candidate(CANDIDATE)
        self.assertEqual(result['module_summary']['count'], 1672)
        self.assertTrue(result['module_summary']['complete_modules_dep_verified'])
        self.assertTrue(result['module_summary']['all_modinfo_dependencies_resolved'])
        self.assertEqual(before, next_full.sha(CANDIDATE / 'manifest.json'))

    def test_real_git_rejects_hidden_flags_changed_bytes_and_lineage(self):
        with tempfile.TemporaryDirectory(prefix='next-source-') as directory:
            work = Path(directory)
            for name in next_full.full.SOURCE_PINS:
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(next_full.WORK / name, path)
            def git(*args):
                return subprocess.check_output(['git', '-C', str(work), *args], text=True, stderr=subprocess.STDOUT).strip()
            git('init', '-q');git('add', 'arch')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture')
            commit = git('rev-parse', 'HEAD')
            with patch.object(next_full, 'COMMIT', commit), patch.object(next_full, 'OFFICIAL_BASE', commit), patch.object(next_full, 'UPSTREAM_OMITTED', {}):
                next_full.source_proof(work, commit)
                name = next(iter(next_full.full.SOURCE_PINS))
                git('update-index', '--assume-unchanged', name)
                with self.assertRaisesRegex(ValueError, 'Hidden Git'):
                    next_full.source_proof(work, commit)
                git('update-index', '--no-assume-unchanged', name)
                (work / name).write_text((work / name).read_text() + '\n# drift\n')
                with self.assertRaisesRegex(ValueError, 'dirty'):
                    next_full.source_proof(work, commit)
                shutil.copyfile(next_full.WORK / name, work / name)
                (work / 'new').write_text('reviewed future extension')
                git('add', 'new');git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'descendant')
                descendant = git('rev-parse', 'HEAD')
                next_full.source_proof(work, descendant)
                disconnected = git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit-tree', 'HEAD^{tree}', '-m', 'unrelated')
                git('checkout', '--detach', disconnected)
                with self.assertRaisesRegex(ValueError, 'descend'):
                    next_full.source_proof(work, disconnected)

    def test_real_module_dependency_and_depmod_failures(self):
        state = json.loads((CANDIDATE / 'manifest.json').read_text())
        library = CANDIDATE / 'modules/lib/modules' / state['kernel_release']
        chosen = next(row for row in state['modules'] if row['depends'])
        source = CANDIDATE / 'modules' / chosen['path']
        with tempfile.TemporaryDirectory(prefix='next-module-') as directory:
            folder = Path(directory);targetlib = folder / 'lib/modules' / state['kernel_release']
            target = targetlib / source.relative_to(library)
            target.parent.mkdir(parents=True);shutil.copyfile(source, target)
            (targetlib / 'modules.dep').write_text(str(target.relative_to(targetlib)) + ':\n')
            with self.assertRaisesRegex(ValueError, 'modinfo module dependency'):
                next_full.validate_modules(folder, state['kernel_release'], required=())
            independent = next(row for row in state['modules'] if not row['depends'])
            target.unlink();source = CANDIDATE / 'modules' / independent['path']
            target = targetlib / source.relative_to(library);target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(source, target)
            (targetlib / 'modules.dep').write_text(str(target.relative_to(targetlib)) + ': kernel/missing.ko\n')
            with self.assertRaisesRegex(ValueError, 'modules.dep dependency'):
                next_full.validate_modules(folder, state['kernel_release'], required=())
            (targetlib / 'modules.dep').write_text('')
            with self.assertRaisesRegex(ValueError, 'Incomplete modules.dep'):
                next_full.validate_modules(folder, state['kernel_release'], required=())
            (targetlib / 'modules.dep').write_text(str(target.relative_to(targetlib)) + ':\n')
            with self.assertRaisesRegex(ValueError, 'Required hardware modules'):
                next_full.validate_modules(folder, state['kernel_release'])

    def test_existing_artifact_and_input_tool_drift_rejected(self):
        with self.assertRaisesRegex(ValueError, 'exists'):
            next_full.reserve_paths(next_full.WORK, ROOT / 'build/kernels/next-full/nonexistent-test', CANDIDATE)
        with tempfile.TemporaryDirectory(prefix='next-input-') as directory:
            tool = Path(directory) / 'tool';tool.write_bytes(b'known tool')
            tools = {'paths': {'tool': str(tool)}, 'sha256': {'tool': next_full.sha(tool)}}
            next_full.check_hashes({}, tools)
            tool.write_bytes(b'drift')
            with self.assertRaisesRegex(ValueError, 'tool drifted'):
                next_full.check_hashes({}, tools)
        with self.assertRaisesRegex(ValueError, 'input drifted'):
            next_full.check_hashes({'configs/linux/piano-full.config': '0' * 64}, {'paths': {}, 'sha256': {}})


if __name__ == '__main__':
    unittest.main()
