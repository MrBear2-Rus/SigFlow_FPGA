import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from sigflow_edu_agent.evaluation import evaluate, load_suite


SUITE = Path(__file__).parent / 'evaluation' / 'development.json'


class EvaluationTests(unittest.TestCase):
    def test_unexpected_probe_values_and_exceptions_do_not_leak_into_report(self):
        suite = load_suite(SUITE)
        suite['cases'] = suite['cases'][:1]
        for value in ({'api_key': 'sk-do-not-log'}, object(), 'sk-do-not-log'):
            with patch('sigflow_edu_agent.evaluation.run_probe', return_value=value):
                report = evaluate(suite)
                self.assertEqual(report['failed'], 1)
                self.assertIsNone(report['results'][0]['actual'])
                self.assertNotIn('sk-do-not-log', json.dumps(report))
        with patch('sigflow_edu_agent.evaluation.run_probe', side_effect=RuntimeError('sk-do-not-log')):
            report = evaluate(suite)
            self.assertEqual(report['failed'], 1)
            self.assertEqual(report['results'][0]['reason'], 'probe_error')
            self.assertNotIn('sk-do-not-log', json.dumps(report))

    def test_development_suite_exercises_security_boundaries(self):
        report = evaluate(load_suite(SUITE))
        self.assertGreaterEqual(report['total'], 30)
        self.assertEqual(report['failed'], 0)
        self.assertEqual(report['passed'], report['total'])
        self.assertEqual(report['evidence_type'], 'synthetic_development')
        self.assertEqual(report['real_model'], 'not_run')
        self.assertEqual(len(report['versions']['source_sha256']), 64)
        self.assertEqual(len({r['case_id'] for r in report['results']}), report['total'])
        self.assertEqual(report['by_execution_kind']['mock_provider']['total'], 3)
        self.assertEqual(sum(v['total'] for v in report['by_execution_kind'].values()), report['total'])
        self.assertEqual(report['by_execution_kind']['real_model']['total'], 0)
        for group in report['by_execution_kind'].values():
            self.assertEqual(group['passed'] + group['failed'], group['total'])

    def test_wrong_expectation_is_reported_as_failure(self):
        suite = load_suite(SUITE)
        suite['cases'] = [dict(suite['cases'][0], expected=False)]
        report = evaluate(suite)
        self.assertEqual((report['total'], report['passed'], report['failed']), (1, 0, 1))
        self.assertFalse(report['results'][0]['passed'])

    def test_suite_rejects_unknown_probe_duplicate_ids_and_non_boolean_expected(self):
        case = load_suite(SUITE)['cases'][0]
        for cases in ([dict(case, probe='shell')], [case, case],
                      [dict(case, expected=1)], [], [dict(case, secret='sk-do-not-log')],
                      [dict(case, case_id='sk-do-not-log\n')]):
            with self.subTest(cases=cases), self.assertRaisesRegex(ValueError, '^invalid_suite$'):
                evaluate({'suite_version': 'development-v1', 'cases': cases})

    def test_cli_nonzero_on_failed_expectation_and_no_raw_input_in_report(self):
        suite = load_suite(SUITE)
        suite['cases'] = [dict(suite['cases'][0], expected=False)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'suite.json'
            path.write_text(json.dumps(suite), encoding='utf-8')
            run = subprocess.run([sys.executable, '-m', 'sigflow_edu_agent.evaluation',
                                  '--suite', str(path)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 1, run.stderr)
        report = json.loads(run.stdout)
        self.assertEqual(report['failed'], 1)
        self.assertNotIn('question', run.stdout)
        self.assertNotIn('api_key', run.stdout.lower())

    def test_invalid_suite_cli_has_redacted_error_and_exit_two(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'suite.json'
            path.write_text('{"secret":"sk-do-not-log"}', encoding='utf-8')
            run = subprocess.run([sys.executable, '-m', 'sigflow_edu_agent.evaluation',
                                  '--suite', str(path)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 2)
        self.assertEqual(json.loads(run.stdout), {'error': 'invalid_suite'})
        self.assertNotIn('sk-do-not-log', run.stdout + run.stderr)

    def test_loader_rejects_oversized_and_invalid_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'suite.json'
            for raw in (b' ' * 65537, b'{', b'\xff'):
                path.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, '^invalid_suite$'):
                    load_suite(path)
