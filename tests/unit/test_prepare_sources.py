"""Check stage-local source selection against the product source lock."""
from pathlib import Path
import unittest

import prepare_sources as sources

ROOT = Path(__file__).resolve().parents[2]


class PrepareSourcesTests(unittest.TestCase):
    def test_stage_groups_cover_the_locked_repositories_once(self):
        locked = sources.expected(ROOT)
        combined = {}
        for group in sources.SOURCE_GROUPS:
            selected = sources.expected(ROOT, group)
            self.assertTrue(selected)
            self.assertFalse(selected.keys() & combined.keys())
            combined.update(selected)
        self.assertEqual(combined, locked)
        self.assertEqual(set(sources.expected(ROOT, 'kernel')), {'upstream/v4l2loopback'})

    def test_nested_repositories_are_fetched_after_their_parent(self):
        for group in ('all', *sources.SOURCE_GROUPS):
            rows = sources.expected(ROOT, group)
            seen = {Path('.')}
            for parent, paths in sources.submodule_batches(rows):
                self.assertIn(parent, seen)
                for path in paths:
                    self.assertNotIn(parent / path, seen)
                    seen.add(parent / path)
            self.assertEqual(seen - {Path('.')}, {Path(name) for name in rows})


if __name__ == '__main__':
    unittest.main()
