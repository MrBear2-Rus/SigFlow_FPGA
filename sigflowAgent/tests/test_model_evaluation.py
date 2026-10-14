from dataclasses import replace
import json
import unittest
from unittest.mock import patch

from sigflow_edu_agent.deepseek import DeepSeekConfig, DeepSeekProvider
from sigflow_edu_agent.domain import Evidence, ProviderError
from sigflow_edu_agent.evaluation.model import ModelCase, run_model_evaluation


class Provider:
    def __init__(self, values=None):
        self.contexts, self.values = [], values

    def generate(self, context):
        self.contexts.append(context)
        if self.values:
            value = self.values.pop(0)
            if isinstance(value, Exception):
                raise value
            return value
        return dict(level=context.level, hint='观察条件覆盖。', question='哪些条件需要检查？',
                    evidence_ids=[context.evidence.evidence_id])


def case(**changes):
    args = dict(case_id='case-latch', question='private question', level=1,
                evidence=Evidence('ev1', 'p1', 'r1', 'job1', 'latch', True, 'private evidence', True))
    return ModelCase(**(args | changes))


class ModelEvaluationTests(unittest.TestCase):
    def run_batch(self, cases=None, provider=None, **changes):
        options = dict(suite_version='frozen-demo-v1', mode='mock', repeats=3,
                       max_model_calls=256, runtime_version='self-developed-v1', rag_version='not_used')
        return run_model_evaluation(cases or (case(),), provider or Provider(), **(options | changes))

    def test_three_runs_have_unique_ids_and_no_raw_input_in_report(self):
        provider = Provider()
        batch = self.run_batch(provider=provider)
        report = batch.report
        self.assertEqual(len(provider.contexts), 3)
        self.assertEqual(report['model_passed'], 3)
        self.assertEqual(len(batch.review_samples), 3)
        self.assertEqual(report['variability'][0]['distinct_output_count'], 1)
        self.assertEqual(report['real_model'], 'not_run')
        self.assertEqual(report['teacher_review'], 'pending')
        self.assertEqual(len({r['sample_id'] for r in report['results']}), 3)
        for text in ('private question', 'private evidence', '观察条件覆盖'):
            self.assertNotIn(text, json.dumps(report, ensure_ascii=False))
        report['results'].clear()
        self.assertEqual(len(batch.report['results']), 3)

    def test_one_repair_then_rule_fallback_and_rejected_candidates_never_exported(self):
        provider = Provider([{'hint': 'secret candidate'}] * 6)
        batch = self.run_batch(provider=provider)
        self.assertEqual(len(provider.contexts), 6)
        self.assertEqual([c.repair for c in provider.contexts], [False, True] * 3)
        self.assertEqual(batch.report['rule_fallbacks'], 3)
        self.assertTrue(all(s.card.producer == 'rule' for s in batch.review_samples))
        self.assertNotIn('secret candidate', json.dumps(batch.report))

    def test_provider_errors_are_redacted_and_not_automatically_retried(self):
        provider = Provider([RuntimeError('secret-key')] * 3)
        batch = self.run_batch(provider=provider)
        self.assertEqual(len(provider.contexts), 3)
        self.assertEqual(batch.report['rule_fallbacks'], 3)
        self.assertNotIn('secret-key', json.dumps(batch.report))
        self.assertEqual({r['reason'] for r in batch.report['results']}, {'model_unavailable'})

    def test_global_budget_never_exceeded_and_preserves_unrun_rows(self):
        provider = Provider()
        batch = self.run_batch(provider=provider, max_model_calls=1)
        self.assertEqual(len(provider.contexts), 1)
        self.assertEqual(batch.report['model_passed'], 1)
        self.assertEqual(batch.report['budget_exhausted'], 2)
        self.assertEqual(batch.report['total'], 3)
        self.assertEqual(len(batch.review_samples), 3)
        self.assertEqual(batch.report['variability'][0]['attempted_repetitions'], 1)
        self.assertFalse(batch.report['model_repetitions_complete'])

    def test_invalid_or_unsupported_evidence_never_reaches_provider(self):
        provider = Provider()
        for evidence in (replace(case().evidence, complete=False), replace(case().evidence, concept='reset')):
            batch = self.run_batch(cases=(case(evidence=evidence),), provider=provider)
            self.assertEqual(batch.report['model_calls'], 0)
        self.assertEqual(provider.contexts, [])

    def test_invalid_parameters_fail_before_calls(self):
        provider = Provider()
        for changes in (dict(repeats=2), dict(repeats=True), dict(repeats=11),
                        dict(max_model_calls=-1), dict(mode='real'), dict(suite_version=''),
                        dict(runtime_version='')):
            with self.assertRaises(ValueError):
                self.run_batch(provider=provider, **changes)
        with self.assertRaises(ValueError):
            self.run_batch(cases=(case(), case()), provider=provider)
        self.assertEqual(provider.contexts, [])

    def test_custom_provider_cannot_be_labeled_deepseek(self):
        with self.assertRaises(ValueError):
            self.run_batch(mode='deepseek')

    @patch('sigflow_edu_agent.deepseek.http.client.HTTPSConnection')
    def test_deepseek_adapter_test_is_explicitly_mock_and_records_actual_config(self, connection):
        reply = connection.return_value.getresponse.return_value
        reply.status = 200
        card = dict(level=1, hint='报告观察到存储。', question='输出需要记忆吗？', evidence_ids=['ev1'])
        reply.read.return_value = json.dumps({'choices': [{'finish_reason': 'stop',
                                                  'message': {'content': json.dumps(card)}}]}).encode()
        provider = DeepSeekProvider(DeepSeekConfig('secret-key', 'test-model', max_tokens=80))
        batch = self.run_batch(provider=provider)
        self.assertEqual(batch.report['parameters']['max_tokens'], 80)
        self.assertIsNone(batch.report['parameters']['temperature'])
        self.assertEqual(batch.report['versions']['model'], 'test-model')
        self.assertEqual(batch.report['model_passed'], 3)
        self.assertEqual(batch.report['real_model'], 'not_run')
        self.assertNotIn('secret-key', json.dumps(batch.report))

    def test_repetition_reports_output_variability_instead_of_best_only(self):
        values = [dict(level=1, hint=hint, question='哪些条件需要检查？', evidence_ids=['ev1'])
                  for hint in ('观察一', '观察二', '观察一')]
        batch = self.run_batch(provider=Provider(values))
        self.assertEqual(batch.report['variability'][0]['distinct_output_count'], 2)
        self.assertEqual(len(batch.report['results']), 3)

    def test_case_shape_and_utf8_are_validated(self):
        for changes in (dict(case_id=''), dict(case_id='../x'), dict(level=4),
                        dict(level=True), dict(question='\ud800')):
            with self.assertRaises(ValueError):
                case(**changes)

    def test_invalid_evidence_origin_never_leaks_into_report(self):
        batch = self.run_batch(cases=(case(evidence=replace(case().evidence, origin='private-origin-secret')),))
        self.assertEqual(batch.report['evidence_rejected'], 3)
        self.assertNotIn('private-origin-secret', json.dumps(batch.report))

    def test_invalid_utf8_output_is_repaired_then_discarded(self):
        values = [dict(level=1, hint='\ud800', question='观察什么？', evidence_ids=['ev1'])] * 6
        batch = self.run_batch(provider=Provider(values))
        self.assertEqual(batch.report['rule_fallbacks'], 3)
        self.assertEqual(batch.report['model_calls'], 6)

    def test_malformed_context_is_rejected_without_provider_calls(self):
        provider = Provider()
        batch = self.run_batch(cases=(case(evidence=replace(case().evidence, context_items=None)),), provider=provider)
        self.assertEqual(batch.report['evidence_rejected'], 3)
        self.assertEqual(provider.contexts, [])
