"""Check the shared official Mesa source and native recipe generation."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from build import mesa


class MesaTests(unittest.TestCase):
    def test_official_pin_is_shared_and_has_no_chip_backport(self):
        data = mesa.policy()
        self.assertEqual(data['source']['version'], '27.0.0-devel')
        self.assertEqual(len(data['source']['commit']), 40)
        self.assertTrue(data['source']['url'].startswith(data['source']['repository'][:-4]))
        self.assertFalse((mesa.ROOT / 'patches/mesa/26.2.6/series.json').exists())

    def test_recipe_retains_all_other_drivers_and_supported_layers(self):
        original = ('GALLIUM_DEFAULTS = softpipe\nGALLIUM_DRIVERS += freedreno llvmpipe softpipe\n'
                    'VULKAN_LAYERS += anti-lag device-select intel-nullhw overlay screenshot\n')
        changed = mesa.debian_rules(original)
        self.assertIn('GALLIUM_DRIVERS += freedreno llvmpipe softpipe', changed)
        self.assertNotIn('GALLIUM_DEFAULTS = softpipe', changed)
        self.assertIn('vram-report-limit', changed)
        self.assertNotIn('anti-lag', changed)

    def test_official_source_integrity_refuses_local_edits(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / 'meson.build'
            source.write_text('official source\n')
            proof = {'official_source': mesa.policy()['source'],
                     'files': {'meson.build': mesa.package_build.sha(source)}}
            mesa.verify_official_source(work, proof)
            source.write_text('modified source\n')
            with self.assertRaisesRegex(ValueError, 'Official Mesa source changed'):
                mesa.verify_official_source(work, proof)


if __name__ == '__main__':
    unittest.main()
