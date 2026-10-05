"""Real sealed module identity and derived-root boundary checks, no device."""
from pathlib import Path
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import stage_piano_kernel_modules as stage


class KernelModuleStageTests(unittest.TestCase):
    def test_actual_complete_kernel_module_manifest_is_verified(self):
        artifact=ROOT/'artifacts/kernels/full-smmu-context'
        if not(artifact/'manifest.json').exists():self.skipTest('Full context kernel artifact unavailable')
        metadata,digest=stage.inspect(artifact)
        self.assertEqual(metadata['source_commit'],'d42158782b81c4aaa47c8643f1400a471785370b')
        self.assertEqual(metadata['module_summary']['count'],1637)
        self.assertEqual(len(digest),64)

    def test_actual_complete_next_module_manifest_is_verified(self):
        artifact=ROOT/'artifacts/kernels/next-full/498569101e34'
        if not(artifact/'manifest.json').exists():self.skipTest('Full Next artifact unavailable')
        metadata,_=stage.inspect(artifact)
        self.assertEqual(metadata['source_commit'],'498569101e34cbb6ec9c27ebe609949279a59bce')
        self.assertEqual(metadata['module_summary']['count'],1672)

    def test_original_base_and_symlink_destinations_are_rejected_before_write(self):
        with self.assertRaisesRegex(ValueError,'derived workspace'):
            stage.stage(ROOT/'artifacts/distros/debian13-arm64-v1/rootfs',ROOT/'artifacts/kernels/full-smmu-context')
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='module-stage-')as temporary:
            root=Path(temporary);(root/'usr').symlink_to('/usr')
            with self.assertRaisesRegex(ValueError,'symlink'):
                stage.safe_target(root,Path('usr/lib/modules'))
            self.assertEqual(list(root.iterdir()),[root/'usr'])


if __name__=='__main__':unittest.main()
