"""Operator file workflow; synthetic ratings below do not certify teachers."""
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from _model_isolation import isolate_model_settings
from test_frozen_model_suite import write_suite
from sigflow_edu_agent.evaluation.model import _hash


class EvaluationRunnerTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        spec = importlib.util.find_spec('sigflow_edu_agent.evaluation.runner')
        self.assertIsNotNone(spec, 'evaluation_operator_entry_missing')
        self.module = importlib.import_module(spec.name)
        self.package = Path(__file__).resolve().parents[1]
        self.directory = tempfile.TemporaryDirectory(dir=self.package)
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)
        self.manifest, self.cases, self.pin = write_suite(self.folder)
        self.output = self.folder/'batch'
        self.review_count = 0
        self.args = ['run', '--manifest', str(self.manifest), '--cases', str(self.cases),
            '--manifest-sha256', self.pin, '--output-dir', str(self.output)]

    def create_batch(self):
        self.assertEqual(self.module.main(self.args), 0)
        return json.loads((self.output/'batch.json').read_text(encoding='utf-8'))

    def review(self, ratings, batch):
        self.review_count += 1
        self.review_file = self.folder/f'review-{self.review_count}.json'
        path = self.folder/'ratings.json'
        path.write_text(json.dumps(ratings), encoding='utf-8')
        return self.module.main(['review', '--batch', str(self.output/'batch.json'),
            '--report-sha256', batch['report_sha256'], '--ratings', str(path),
            '--output', str(self.review_file)])

    def test_rule_batch_exports_bound_checked_cards_and_unfilled_blind_materials(self):
        batch = self.create_batch()
        report = batch['report']
        self.assertEqual((report['execution_kind'], report['model_calls'], report['total']), ('rule', 0, 3))
        self.assertEqual(report['teacher_review'], 'pending')
        packet = json.loads((self.output/'review-packet.json').read_text(encoding='utf-8'))
        raw = json.dumps(packet)
        for private in ('"producer"', '"model"', '"base_url"', '"execution_kind"'):
            self.assertNotIn(private, raw)
        self.assertEqual(len(packet['samples']), 3)
        ratings = json.loads((self.output/'ratings-template.json').read_text(encoding='utf-8'))
        self.assertEqual(len(ratings), 6)
        self.assertTrue(all(rating['correctness'] is None for rating in ratings))
        self.assertEqual(self.review([], batch), 1)
        self.assertEqual(json.loads(self.review_file.read_text())['status'], 'incomplete')

    def test_supplied_scores_roundtrip_and_severe_findings_block_threshold(self):
        batch = self.create_batch()
        ratings = json.loads((self.output/'ratings-template.json').read_text(encoding='utf-8'))
        for rating in ratings:
            for key in ('correctness', 'progression', 'actionability', 'evidence'):
                rating[key] = 2
        self.assertEqual(self.review(ratings, batch), 0)
        report = json.loads(self.review_file.read_text())
        self.assertEqual(report['status'], 'threshold_met')
        self.assertEqual(report['product_acceptance'], 'not_determined')
        ratings[0]['severe_issues'] = ['hardware_error']
        self.assertEqual(self.review(ratings, batch), 1)
        self.assertEqual(json.loads(self.review_file.read_text())['status'], 'needs_remediation')

    def test_tampered_checked_card_is_rejected_on_reopen(self):
        batch = self.create_batch()
        batch['samples'][0]['card']['hint'] = 'forged card'
        (self.output/'batch.json').write_text(json.dumps(batch), encoding='utf-8')
        self.assertEqual(self.review([], batch), 2)
        self.assertFalse(self.review_file.exists())

    def test_blind_packet_and_ratings_do_not_reveal_enumerable_producer_hash(self):
        self.create_batch()
        packet = json.loads((self.output/'review-packet.json').read_text(encoding='utf-8'))
        ratings = json.loads((self.output/'ratings-template.json').read_text(encoding='utf-8'))
        by_sample = {sample['sample_id']:sample for sample in packet['samples']}
        for sample in packet['samples']:
            with self.subTest(sample_id=sample['sample_id']):
                self.assertEqual(sample['card_sha256'], _hash(sample['card']))
                for producer in ('rule', 'model'):
                    self.assertNotEqual(sample['card_sha256'], _hash(sample['card'] | {'producer':producer}))
        for rating in ratings:
            self.assertEqual(rating['card_sha256'], by_sample[rating['sample_id']]['card_sha256'])

    def test_rating_for_altered_public_card_is_rejected_before_aggregation(self):
        batch = self.create_batch()
        ratings = json.loads((self.output/'ratings-template.json').read_text(encoding='utf-8'))
        for rating in ratings:
            for key in ('correctness', 'progression', 'actionability', 'evidence'):
                rating[key] = 2
        packet = json.loads((self.output/'review-packet.json').read_text(encoding='utf-8'))
        sample = packet['samples'][0]
        altered_hash = _hash(sample['card'] | {'hint':'altered public review content'})
        for rating in ratings:
            if rating['sample_id'] == sample['sample_id']:
                rating['card_sha256'] = altered_hash
        self.assertEqual(self.review(ratings, batch), 2)
        self.assertFalse(self.review_file.exists())

    def test_private_producer_inclusive_hash_cannot_substitute_for_public_rating(self):
        batch = self.create_batch()
        ratings = json.loads((self.output/'ratings-template.json').read_text(encoding='utf-8'))
        private_hashes = {sample['sample_id']:_hash(sample['card']) for sample in batch['samples']}
        for rating in ratings:
            rating['card_sha256'] = private_hashes[rating['sample_id']]
            for key in ('correctness', 'progression', 'actionability', 'evidence'):
                rating[key] = 2
        self.assertEqual(self.review(ratings, batch), 2)
        self.assertFalse(self.review_file.exists())

    def test_existing_batch_directory_is_not_overwritten(self):
        self.create_batch()
        path = self.output/'batch.json'
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual(self.module.main(self.args), 2)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)

    def test_real_subprocess_default_needs_no_key_and_pin_error_precedes_provider(self):
        environment = dict(os.environ, PYTHONPATH=str(self.package))
        environment.pop('DEEPSEEK_API_KEY', None)
        command = [sys.executable, '-m', 'sigflow_edu_agent.evaluation.runner']
        result = subprocess.run(command+self.args, capture_output=True, env=environment, cwd=self.package, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.output/'batch.json').read_text())['report']['real_model'], 'not_run')
        wrong = self.args.copy()
        wrong[wrong.index(self.pin)] = '0'*64
        result = subprocess.run(command+wrong+['--mode', 'deepseek'], capture_output=True,
            env=environment, cwd=self.package, timeout=8)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(str(self.folder).encode(), result.stdout+result.stderr)
