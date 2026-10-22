"""Frozen inputs are externally pinned declarations, not teacher acceptance."""
from dataclasses import asdict
import hashlib
import importlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

from sigflow_edu_agent.demo import FixtureSource


def write_suite(directory, *, rows=None):
    directory = Path(directory)
    case = dict(case_id='case-latch', question='Explain the bounded observation', level=1,
                evidence=asdict(FixtureSource().collect('p', 'r')))
    rows = [case] if rows is None else rows
    raw = json.dumps(rows, ensure_ascii=True).encode()
    cases = directory/'cases.json'
    cases.write_bytes(raw)
    manifest = dict(schema_version='edu.frozen-model-suite.v1', suite_version='external-test-v1',
        suite_sha256=hashlib.sha256(raw).hexdigest(), case_count=len(rows),
        reviewers=['reviewer-one', 'reviewer-two'])
    path = directory/'manifest.json'
    path.write_text(json.dumps(manifest), encoding='utf-8')
    return path, cases, hashlib.sha256(path.read_bytes()).hexdigest()


class FrozenModelSuiteTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.find_spec('sigflow_edu_agent.evaluation.frozen')
        self.assertIsNotNone(spec, 'frozen_model_case_loader_missing')
        self.module = importlib.import_module(spec.name)
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.directory.cleanup)

    def load(self, files):
        manifest, cases, pin = files
        return self.module.load_frozen_model_suite(manifest, cases, expected_manifest_sha256=pin)

    def test_pinned_input_reconstructs_typed_cases_and_retains_fixture_origin(self):
        files = write_suite(self.directory.name)
        suite = self.load(files)
        self.assertEqual(suite.suite_version, 'external-test-v1')
        self.assertEqual(suite.reviewers, ('reviewer-one', 'reviewer-two'))
        self.assertEqual(suite.cases[0].evidence.origin, 'fixture')
        self.assertEqual((suite.cases[0].level, suite.cases[0].evidence.project_id), (1, 'p'))
        self.assertEqual(suite.manifest_sha256, files[2])

    def test_manifest_and_suite_tampering_are_rejected(self):
        for target in (0, 1):
            files = write_suite(self.directory.name)
            files[target].write_bytes(files[target].read_bytes()+b' ')
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, 'invalid_frozen_model_suite'):
                self.load(files)

    def test_unknown_fields_unsafe_types_and_duplicate_cases_are_rejected(self):
        valid = json.loads(write_suite(self.directory.name)[1].read_text())[0]
        import copy
        for mutate in (lambda c:c.update(extra='unexpected'), lambda c:c.update(level=True),
                       lambda c:c['evidence'].update(complete=1), lambda c:c['evidence'].update(origin='production'),
                       lambda c:c['evidence'].update(anchor={'artifact_id':'missing-scope'}),
                       lambda c:c['evidence'].update(context_items=[{'text':'missing-scope'}]),
                       lambda c:c.update(question='x'*4097)):
            case = copy.deepcopy(valid)
            mutate(case)
            with self.subTest(case=case['case_id']), self.assertRaisesRegex(ValueError, 'invalid_frozen_model_suite'):
                self.load(write_suite(self.directory.name, rows=[case]))
        with self.assertRaisesRegex(ValueError, 'invalid_frozen_model_suite'):
            self.load(write_suite(self.directory.name, rows=[valid, valid]))

    def test_duplicate_keys_nonfinite_and_oversized_files_are_rejected(self):
        files = write_suite(self.directory.name)
        for raw in (b'{"schema_version":1,"schema_version":2}', b'{"value":NaN}', b'x'*16385):
            files[0].write_bytes(raw)
            with self.subTest(size=len(raw)), self.assertRaisesRegex(ValueError, 'invalid_frozen_model_suite'):
                self.module.load_frozen_model_suite(files[0], files[1], expected_manifest_sha256=hashlib.sha256(raw).hexdigest())

    def test_nested_anchor_context_and_omissions_are_typed_without_execution(self):
        case = json.loads(write_suite(self.directory.name)[1].read_text())[0]
        evidence = case['evidence']
        evidence['anchor'] = dict(evidence_id=evidence['evidence_id'], project_id='p', revision='r',
            job_id=evidence['job_id'], snapshot_id='s', artifact_id='a', artifact_hash='a'*64, expired=False,
            source_ref=dict(project_id='p', revision='r', source_id='src', file_hash='b'*64,
                start_line=1, end_line=2, start_column=None, end_column=None))
        evidence['context_items'] = [dict(item_id='src', kind='source', project_id='p', revision='r', text='bounded data')]
        evidence['omitted'] = [dict(item_id='course:1', reason='budget')]
        result = self.load(write_suite(self.directory.name, rows=[case])).cases[0].evidence
        self.assertEqual(result.anchor.source_ref.start_line, 1)
        self.assertEqual(result.context_items[0].text, 'bounded data')
        self.assertEqual(result.omitted[0].reason, 'budget')
