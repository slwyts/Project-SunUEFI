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
    def invoke(self, group, result, missing=False, *, flags=()):
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
                code = RUNNER.main(['--group', group, *flags])
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
        for group, expected in (('portable', 1), ('ci', 1), ('python-all', 0)):
            code, output, _ = self.invoke(group, result)
            self.assertEqual(code, expected)
            self.assertIn('1 skipped', output)
            self.assertIn('optional check skipped', output)

    def test_release_checks_reject_skips_and_missing_test_patterns(self):
        result = ('SKIPPED', 1, 1, 'native tool unavailable\n')
        code, output, _ = self.invoke('python-all', result, flags=['--no-skips'])
        self.assertEqual(code, 1)
        self.assertIn('FAILED tests/unit/test_fixture.py', output)
        self.assertIn('native tool unavailable', output)
        with contextlib.redirect_stderr(io.StringIO()) as output, \
                self.assertRaises(SystemExit) as error:
            self.invoke('python-all', result, flags=[
                '--no-skips', '--pattern', 'test_fixture.py', '--pattern', 'test_missing.py'])
        self.assertEqual(error.exception.code, 2)
        self.assertIn("No check matches 'test_missing.py'", output.getvalue())

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

    def test_ci_runs_python_and_shell_checks_without_duplicates(self):
        files = RUNNER.checks(ROOT, 'ci')
        self.assertEqual(len(files), len(set(files)))
        for name in ('tests/unit/test_boot_options.py', 'tests/unit/test_camera.py',
                     'tests/unit/test_nv_journal.py', 'tests/unit/test_nv_runtime_guard.py',
                     'tests/unit/test_reset.py', 'tests/unit/test_rtc_offset.py',
                     'tests/native/test_fastboot_launch.sh'):
            self.assertIn(ROOT / name, files)
        self.assertNotIn(ROOT / 'tests/native/test_ufs_bounded_fs.sh', files)

    def test_native_script_failures_and_skips_are_reported(self):
        path = Path('/tmp/test_fixture.sh')
        for code, stdout, expected in ((0, 'OK\n', 'PASSED'),
                                       (1, 'failure\n', 'FAILED'),
                                       (0, 'Skipped: input missing\n', 'SKIPPED')):
            with self.subTest(expected=expected), patch.object(
                    RUNNER.subprocess, 'run',
                    return_value=subprocess.CompletedProcess([], code, stdout=stdout)) as run:
                self.assertEqual(RUNNER.run_check(path, 1)[0], expected)
                self.assertEqual(run.call_args.args[0], ['bash', str(path)])


if __name__ == '__main__':
    unittest.main()
