"""Verify the CANoe versioned report without installed applications."""
import contextlib
import io
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import installed_matrix


class InstalledMatrixTest(unittest.TestCase):
    def run_matrix(self, versions, *, failed_case=False, missing_config=False):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = root / 'evidence'
            config = root / 'fixture.cfg'
            config.touch()
            arguments = ['installed_matrix.py', '--package-root', 'package', '--runtime-root', 'runtime',
                         '--evidence-directory', str(evidence), '--pcan-endpoint-id', 'endpoint',
                         '--endpoint-label', 'fixture', '--vector-peer-channel', '0']
            for version in versions:
                arguments.extend([f'--canoe{version}-classic-config', str(config)])
                if not missing_config:
                    arguments.extend([f'--canoe{version}-fd-config', str(config)])
            with patch.object(sys, 'argv', arguments), contextlib.redirect_stdout(io.StringIO()), \
                    patch.object(installed_matrix, 'installed_executable',
                                 side_effect=lambda major, override: root / str(major) if major in versions else None), \
                    patch.object(installed_matrix, 'check_version'), \
                    patch.object(installed_matrix, 'run_case', return_value={
                        'status': 'failed' if failed_case else 'passed'}) as run_case:
                code = installed_matrix.main()
            report = json.loads((evidence / 'result.json').read_text(encoding='utf-8'))
            return code, report, run_case.call_args_list

    def test_runs_each_installed_version_and_skips_the_other(self):
        for installed in (11, 12):
            with self.subTest(installed=installed):
                code, report, calls = self.run_matrix({installed})
                self.assertEqual((code, report['status']), (0, 'passed'))
                subsets = {subset['version']: subset for subset in report['subsets']}
                self.assertEqual(subsets[installed]['status'], 'passed')
                absent = 23 - installed
                self.assertEqual(subsets[absent]['status'], 'skipped')
                self.assertIn(f'CANoe {absent} x64 is not installed', subsets[absent]['reason'])
                self.assertEqual(len(calls), 4)
                inputs = [call.args[0] for call in calls]
                self.assertEqual({(args[args.index('--mode') + 1], args[args.index('--local-app-data-min-length') + 1])
                                  for args in inputs}, {('classic', '0'), ('classic', '111'), ('fd', '0'), ('fd', '111')})

    def test_both_absent_skip_the_matrix(self):
        code, report, calls = self.run_matrix(set())
        self.assertEqual((code, report['status']), (0, 'skipped'))
        self.assertEqual([subset['status'] for subset in report['subsets']], ['skipped', 'skipped'])
        self.assertEqual(calls, [])

    def test_both_installed_run_both_subsets(self):
        code, report, calls = self.run_matrix({11, 12})
        self.assertEqual((code, report['status']), (0, 'passed'))
        self.assertEqual([subset['status'] for subset in report['subsets']], ['passed', 'passed'])
        self.assertEqual(len(calls), 8)

    def test_installed_version_failure_fails_the_matrix(self):
        code, report, calls = self.run_matrix({12}, failed_case=True)
        self.assertEqual((code, report['status']), (1, 'failed'))
        self.assertEqual([subset['status'] for subset in report['subsets']], ['skipped', 'failed'])
        self.assertEqual(len(calls), 4)

    def test_installed_version_without_configuration_fails(self):
        code, report, calls = self.run_matrix({12}, missing_config=True)
        self.assertEqual((code, report['status']), (1, 'failed'))
        self.assertIn('--canoe12-fd-config', report['subsets'][1]['reason'])
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
