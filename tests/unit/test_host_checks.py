"""Offline runner behavior: failures, missing files and skipped coverage."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('host_checks', ROOT / 'tools/check_host.py')
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class HostChecksTests(unittest.TestCase):
    def invoke(self, group, result, missing=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tests/unit').mkdir(parents=True)
            path = root / 'tests/unit/test_fixture.py'
            if not missing:
                path.write_text('fixture')
            with patch.object(RUNNER, 'ROOT', root), \
                    patch.object(RUNNER, 'checks', return_value=[path]), \
                    patch.object(RUNNER, 'run_check', return_value=result) as run, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                code = RUNNER.main(['--group', group])
            return code, output.getvalue(), run.call_count

    def test_failed_and_missing_checks_cannot_report_success(self):
        for missing in (False, True):
            with self.subTest(missing=missing):
                code, output, calls = self.invoke('portable', ('FAILED', 1, 0, 'failure'), missing)
                self.assertEqual(code, 1)
                self.assertIn('1 failing files', output)
                self.assertEqual(calls, 0 if missing else 1)

    def test_portable_skips_fail_but_optional_group_reports_them(self):
        result = ('PASSED_WITH_SKIPS', 2, 1, 'one optional check skipped\n')
        for group, expected in (('portable', 1), ('python-all', 0)):
            code, output, _ = self.invoke(group, result)
            self.assertEqual(code, expected)
            self.assertIn('1 skipped', output)
            self.assertIn('optional check skipped', output)

    def test_discovery_includes_unit_files_only_and_list_never_executes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ('tools', 'tests/unit', 'tests/native'):
                (root / folder).mkdir(parents=True)
                (root / folder / 'test_example.py').touch()
                (root / folder / 'normal.py').touch()
            self.assertEqual(len(RUNNER.checks(root, 'python-all')), 1)
            with patch.object(RUNNER, 'ROOT', root), patch.object(RUNNER, 'run_check') as run, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(RUNNER.main(['--group', 'python-all', '--list']), 0)
            run.assert_not_called()
            self.assertNotIn('tools/test_example.py', output.getvalue())
            self.assertNotIn('tests/native/test_example.py', output.getvalue())
            self.assertIn('tests/unit/test_example.py', output.getvalue())

    def test_timeout_and_non_test_output_are_failures(self):
        with patch.object(RUNNER.subprocess, 'run', side_effect=subprocess.TimeoutExpired('test', 1)):
            self.assertEqual(RUNNER.run_check(Path('/tmp/test_fixture.py'), 1)[0], 'FAILED')
        response = subprocess.CompletedProcess([], 0, stdout='No tests were run\n')
        with patch.object(RUNNER.subprocess, 'run', return_value=response):
            self.assertEqual(RUNNER.run_check(Path('/tmp/test_fixture.py'), 1)[0], 'FAILED')


if __name__ == '__main__':
    unittest.main()
