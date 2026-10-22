import hashlib,json,tempfile,unittest
from pathlib import Path

class EvaluationHandoffTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.suite=self.root/'suite.json';self.manifest=self.root/'manifest.json'
        self.suite.write_text(json.dumps(dict(suite_version='development-v1',cases=[dict(case_id='dev-001',probe='reference_request',expected=True)])))
        self.value=dict(schema_version='edu.frozen-probe-suite.v1',suite_version='team-v1',
            suite_sha256=hashlib.sha256(self.suite.read_bytes()).hexdigest(),reviewers=['teacher1','teacher2'])
    def write(self):
        self.manifest.write_text(json.dumps(self.value));return hashlib.sha256(self.manifest.read_bytes()).hexdigest()
    def load(self,digest):
        from sigflow_edu_agent.evaluation.handoff import load_frozen_suite
        return load_frozen_suite(self.manifest,self.suite,expected_manifest_sha256=digest)
    def test_pinned_manifest_and_raw_suite_hash_are_verified(self):
        loaded=self.load(self.write());self.assertEqual(loaded['suite_version'],'development-v1')
    def test_modified_suite_or_manifest_is_denied(self):
        digest=self.write();self.suite.write_text('{}')
        with self.assertRaises(ValueError):self.load(digest)
        self.manifest.write_text('{}')
        with self.assertRaises(ValueError):self.load(digest)
    def test_empty_frozen_set_and_single_or_duplicate_reviewer_are_denied(self):
        for reviewers in ([],['one'],['one','one']):
            self.value['reviewers']=reviewers
            with self.subTest(reviewers=reviewers),self.assertRaises(ValueError):self.load(self.write())
        self.value['reviewers']=['one','two'];self.suite.write_text('')
        self.value['suite_sha256']=hashlib.sha256(b'').hexdigest()
        with self.assertRaises(ValueError):self.load(self.write())
    def test_duplicate_json_keys_and_invalid_probe_are_denied(self):
        self.suite.write_text('{"suite_version":"development-v1","suite_version":"other","cases":[]}')
        self.value['suite_sha256']=hashlib.sha256(self.suite.read_bytes()).hexdigest()
        with self.assertRaises(ValueError):self.load(self.write())
        self.suite.write_text(json.dumps(dict(suite_version='development-v1',cases=[dict(case_id='dev-001',probe='untrusted',expected=True)])))
        self.value['suite_sha256']=hashlib.sha256(self.suite.read_bytes()).hexdigest()
        with self.assertRaises(ValueError):self.load(self.write())
    def test_handoff_report_never_promotes_development_to_teacher_acceptance(self):
        from sigflow_edu_agent.evaluation.handoff import build_handoff
        root=Path(__file__).resolve().parent/'evaluation'
        report=build_handoff(root/'development.json',root/'fixtures_frozen.jsonl')
        self.assertEqual((report['development']['total'],report['development']['failed']),(40,0))
        self.assertEqual(report['frozen_dataset']['status'],'missing')
        self.assertEqual(report['team_gate'],'pending_external_signoff')
        self.assertEqual(report['real_model'],'not_run');self.assertEqual(report['teacher_review'],'not_run')
    def test_matrix_keeps_warning_labels_separate_from_proven_hardware_roots(self):
        from sigflow_edu_agent.patterns.support import support_matrix
        rows=support_matrix();self.assertEqual(len(rows),8)
        by={r['concept']:r for r in rows}
        self.assertFalse(by['latch']['production_detector'])
        self.assertTrue(by['latch']['report_adapter_available'])
        self.assertEqual(by['latch']['producer_status'], 'external_delivery_required')
        self.assertFalse(by['width_signedness']['production_detector'])
        self.assertEqual(by['simulation_mismatch']['bounded_observation'],'explicit_point_comparison')
        self.assertFalse(by['reset']['production_detector'])
