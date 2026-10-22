"""Synthetic inputs exercise offline readiness, never team or teacher acceptance."""
from contextlib import redirect_stdout
from copy import deepcopy
from dataclasses import asdict
import hashlib
import importlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from sigflow_edu_agent.demo import FixtureSource


class EvaluationPreflightTests(unittest.TestCase):
    def setUp(self):
        self.package = Path(__file__).resolve().parents[1]
        self.directory = tempfile.TemporaryDirectory(dir=self.package)
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)
        spec = importlib.util.find_spec('sigflow_edu_agent.evaluation.preflight')
        self.assertIsNotNone(spec, 'offline_evaluation_preflight_missing')
        self.module = importlib.import_module(spec.name)

    def suite(self, count=40, mutate=None, manifest_mutate=None):
        # Deliberately synthetic and permanently fixture-labelled. No checked-in
        # team dataset is created or claimed by these unit tests.
        evidence = asdict(FixtureSource().collect('p', 'r'))
        rows = [dict(case_id=f'case-{number}', question='bounded synthetic question',
                     level=1, evidence=deepcopy(evidence)) for number in range(count)]
        if mutate:
            mutate(rows)
        raw = json.dumps(rows).encode()
        cases = self.folder/'cases.json'
        cases.write_bytes(raw)
        manifest = dict(schema_version='edu.frozen-model-suite.v1', suite_version='synthetic-test-v1',
            suite_sha256=hashlib.sha256(raw).hexdigest(), case_count=count,
            reviewers=['reviewer-one', 'reviewer-two'])
        if manifest_mutate:
            manifest_mutate(manifest)
        path = self.folder/'manifest.json'
        path.write_text(json.dumps(manifest), encoding='utf-8')
        return path, cases, hashlib.sha256(path.read_bytes()).hexdigest()

    def run_cli(self, files=None, *extra):
        args = []
        if files:
            manifest, cases, pin = files
            args = ['--manifest', str(manifest), '--cases', str(cases), '--manifest-sha256', pin]
        output = io.StringIO()
        with redirect_stdout(output):
            code = self.module.main(args+list(extra))
        return code, json.loads(output.getvalue())

    def test_correct_pin_and_sufficient_budget_preserve_fixture_origin(self):
        code, report = self.run_cli(self.suite(), '--max-model-calls', '240')
        self.assertEqual(code, 0)
        self.assertTrue(report['can_run_real_batch'])
        self.assertEqual(report['origin_counts'], dict(fixture=40, tool=0, rule=0))
        self.assertEqual(report['case_counts'], dict(total=40, eligible=40, evidence_rejected=0, unsupported_pattern=0))
        self.assertEqual(report['budget'], dict(repeats=3, max_model_calls=240,
            required_first_pass=120, required_with_repair=240, first_pass_sufficient=True,
            with_repair_sufficient=True))
        self.assertEqual(report['teacher_review'], 'pending')
        self.assertEqual(report['review_authenticity'], 'external_signoff_required')
        self.assertEqual(report['product_acceptance'], 'not_determined')
        self.assertEqual(report['real_model'], 'not_run')

    def test_missing_input_stays_blocked_without_substituting_probe_fixtures(self):
        code, report = self.run_cli()
        self.assertEqual(code, 1)
        self.assertFalse(report['can_run_real_batch'])
        self.assertEqual(report['input_status'], 'not_provided')
        self.assertEqual(report['issues'], ['frozen_model_suite_not_provided'])

    def test_wrong_pin_tampered_cases_and_jsonl_fail_closed_without_echo(self):
        files = self.suite()
        for wrong in ((files[0], files[1], '0'*64),
                      (files[0], self.folder/'private-sk-secret.jsonl', files[2])):
            code, report = self.run_cli(wrong)
            self.assertEqual(code, 2)
            self.assertEqual(report['issues'], ['invalid_frozen_model_suite'])
            self.assertNotIn('private-sk-secret', json.dumps(report))
        files[1].write_bytes(files[1].read_bytes()+b' ')
        self.assertEqual(self.run_cli(files)[0], 2)

    def test_jsonl_probe_rows_cannot_substitute_for_frozen_model_cases(self):
        files = self.suite()
        # Correctly pin the JSONL bytes to prove format/shape rejection rather
        # than an incidental digest mismatch or absent-file error.
        raw = b'{"probe":"synthetic-only"}\n{"probe":"synthetic-only"}\n'
        files[1].write_bytes(raw)
        manifest = json.loads(files[0].read_text(encoding='utf-8'))
        manifest['suite_sha256'] = hashlib.sha256(raw).hexdigest()
        files[0].write_text(json.dumps(manifest), encoding='utf-8')
        pin = hashlib.sha256(files[0].read_bytes()).hexdigest()
        code, report = self.run_cli((files[0], files[1], pin))
        self.assertEqual(code, 2)
        self.assertFalse(report['can_run_real_batch'])

    def test_team_count_and_conservative_repair_budget_gate_readiness(self):
        code, report = self.run_cli(self.suite(39), '--max-model-calls', '234')
        self.assertEqual(code, 1)
        self.assertIn('team_case_count_below_40', report['issues'])
        files = self.suite()
        for budget, first, repair in ((119, False, False), (120, True, False),
                                     (239, True, False), (240, True, True)):
            with self.subTest(budget=budget):
                code, report = self.run_cli(files, '--max-model-calls', str(budget))
                self.assertEqual(code, 0 if repair else 1)
                self.assertEqual(report['budget']['first_pass_sufficient'], first)
                self.assertEqual(report['budget']['with_repair_sufficient'], repair)

    def test_rejected_and_unsupported_cases_do_not_inflate_model_call_budget(self):
        def mutate(rows):
            rows[0]['evidence']['complete'] = False
            rows[1]['evidence']['concept'] = 'syntax'
            rows[2]['evidence']['origin'] = 'rule'
        code, report = self.run_cli(self.suite(mutate=mutate), '--max-model-calls', '228')
        self.assertEqual(code, 1)
        self.assertEqual(report['case_counts'], dict(total=40, eligible=38, evidence_rejected=1, unsupported_pattern=1))
        self.assertEqual(report['budget']['required_first_pass'], 114)
        self.assertEqual(report['budget']['required_with_repair'], 228)
        self.assertEqual(report['origin_counts'], dict(fixture=39, tool=0, rule=1))
        self.assertIn('evidence_rejected', report['issues'])
        self.assertIn('unsupported_pattern', report['issues'])

    def test_versions_reviewers_and_unsafe_case_fields_must_be_valid(self):
        mutations = [lambda m:m.update(suite_version=''), lambda m:m.update(reviewers=[]),
                     lambda m:m.update(reviewers=['reviewer-one', 'reviewer-one'])]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assertEqual(self.run_cli(self.suite(manifest_mutate=mutation))[0], 2)
        for mutation in (lambda rows:rows[0].update(level=True),
                         lambda rows:rows[0]['evidence'].update(origin='production'),
                         lambda rows:rows[1].update(case_id='case-0')):
            with self.subTest(mutation=mutation):
                self.assertEqual(self.run_cli(self.suite(mutate=mutation))[0], 2)

    def test_config_bounds_modes_and_course_version_are_explicit(self):
        files = self.suite()
        for option, value in (('--repeats', '2'), ('--repeats', '11'), ('--max-model-calls', '0'),
                              ('--max-model-calls', '513'), ('--repeats', 'secret-sk-value'),
                              ('--runtime-version', '../secret-sk-value'), ('--unknown-secret', 'x')):
            with self.subTest(option=option, value=value):
                code, report = self.run_cli(files, option, value)
                self.assertEqual(code, 2)
                self.assertFalse(report['can_run_real_batch'])
                self.assertNotIn('secret-sk-value', json.dumps(report))
        code, report = self.run_cli(files, '--mode', 'rule')
        self.assertEqual(code, 1)
        self.assertIn('unsupported_execution_mode', report['issues'])
        files = self.suite(mutate=lambda rows:rows[0]['evidence'].update(context_items=[dict(
            item_id='course:1', kind='course', project_id='p', revision='r', text='bounded course')]))
        code, report = self.run_cli(files)
        self.assertEqual(code, 1)
        self.assertIn('rag_version_required', report['issues'])
        self.assertEqual(self.run_cli(files, '--rag-version', 'course-v1')[0], 0)

    def test_numeric_boundary_values_and_typed_api_reject_boolean_integers(self):
        files = self.suite()
        code, report = self.run_cli(files, '--repeats', '10', '--max-model-calls', '512')
        self.assertEqual(code, 1)
        self.assertEqual(report['budget']['required_first_pass'], 400)
        self.assertEqual(report['budget']['required_with_repair'], 800)
        self.assertTrue(report['budget']['first_pass_sufficient'])
        self.assertFalse(report['budget']['with_repair_sufficient'])
        self.assertEqual(self.run_cli(files, '--repeats', '6', '--max-model-calls', '512')[0], 0)
        self.assertEqual(self.run_cli(files, '--max-model-calls', '1')[0], 1)
        for changes in (dict(repeats=True), dict(max_model_calls=True), dict(rag_version=None)):
            with self.subTest(changes=changes):
                report = self.module.preflight(files[0], files[1], expected_manifest_sha256=files[2], **changes)
                self.assertEqual(report['status'], 'invalid_input')

    def test_subprocess_without_keys_denies_settings_network_and_model_execution(self):
        files = self.suite()
        args = ['--manifest', str(files[0]), '--cases', str(files[1]), '--manifest-sha256', files[2]]
        script = '''
import json, runpy, socket, sys, urllib.request
from unittest.mock import patch
from sigflow_edu_agent.deepseek import DeepSeekProvider
from sigflow_edu_agent.evaluation import model
allowed = {sys.argv[1], sys.argv[2]}
def audit(event, values):
    if event.startswith('socket.') or (event == 'open' and isinstance(values[0], str)
            and (values[0].endswith('.json') or values[0].endswith('.jsonl')) and values[0] not in allowed):
        raise RuntimeError('forbidden_external_access')
sys.addaudithook(audit)
sys.argv = ['preflight'] + sys.argv[3:]
with patch.object(DeepSeekProvider, 'from_settings', side_effect=RuntimeError('settings_read')), \\
     patch.object(model, 'run_model_evaluation', side_effect=RuntimeError('model_run')), \\
     patch.object(socket, 'socket', side_effect=RuntimeError('network')), \\
     patch.object(urllib.request, 'urlopen', side_effect=RuntimeError('network')):
    runpy.run_module('sigflow_edu_agent.evaluation.preflight', run_name='__main__')
'''
        environment = {key:value for key, value in os.environ.items()
                       if not key.startswith(('DEEPSEEK_', 'SIGFLOW_DEEPSEEK'))}
        environment['PYTHONPATH'] = str(self.package)
        result = subprocess.run([sys.executable, '-c', script, str(files[0]), str(files[1])]+args,
            cwd=self.package, env=environment, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report['can_run_real_batch'])
        self.assertEqual(report['real_model'], 'not_run')
        self.assertEqual(result.stderr, b'')

    def test_sensitive_question_summary_and_path_never_enter_summary(self):
        secret = 'sk-synthetic-DO-NOT-PRINT'
        files = self.suite(mutate=lambda rows:rows[0].update(question=secret))
        code, report = self.run_cli(files)
        self.assertEqual(code, 0)
        raw = json.dumps(report)
        self.assertNotIn(secret, raw)
        self.assertNotIn(str(self.folder), raw)
        self.assertNotIn('bounded synthetic', raw)
        self.assertNotIn('reviewer-one', raw)


if __name__ == '__main__':
    unittest.main()
