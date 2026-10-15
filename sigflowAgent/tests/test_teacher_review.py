from dataclasses import replace
import unittest

from sigflow_edu_agent.evaluation.review import TeacherRating, aggregate_teacher_reviews
from test_model_evaluation import case, Provider
from sigflow_edu_agent.evaluation.model import run_model_evaluation


class TeacherReviewTests(unittest.TestCase):
    def setUp(self):
        self.batch = run_model_evaluation((case(),), Provider(), suite_version='demo-v1', mode='mock')
        self.ratings = []
        for row in self.batch.report['results']:
            for reviewer in ('reviewer-a', 'reviewer-b'):
                self.ratings.append(TeacherRating(self.batch.report_sha256, row['sample_id'], row['output_sha256'],
                                                  reviewer, 2, 2, 2, 2))

    def test_two_complete_reviews_meet_threshold_but_do_not_claim_real_acceptance(self):
        result = aggregate_teacher_reviews(self.batch, self.ratings)
        self.assertEqual(result['status'], 'threshold_met')
        self.assertEqual(result['mean_score'], 8)
        self.assertEqual(result['sample_count'], 3)
        self.assertEqual(result['source_execution_kind'], 'mock_provider')
        self.assertEqual(result['product_acceptance'], 'not_determined')
        self.assertEqual(len(result['ratings']), 6)

    def test_missing_reviewer_or_sample_is_incomplete(self):
        for ratings in ([], self.ratings[:-1], [r for r in self.ratings if r.reviewer_id == 'reviewer-a']):
            result = aggregate_teacher_reviews(self.batch, ratings)
            self.assertEqual(result['status'], 'incomplete')
            self.assertFalse(result['threshold_met'])
            self.assertTrue(result['missing'])

    def test_severe_issue_cannot_be_hidden_by_high_mean(self):
        ratings = list(self.ratings)
        ratings[0] = replace(ratings[0], severe_issues=('answer_leak',))
        result = aggregate_teacher_reviews(self.batch, ratings)
        self.assertEqual(result['mean_score'], 8)
        self.assertEqual(result['status'], 'needs_remediation')
        self.assertFalse(result['threshold_met'])
        self.assertEqual(result['severe_issue_count'], 1)

    def test_low_mean_and_disagreements_are_retained(self):
        ratings = [replace(r, correctness=0, progression=0, actionability=0, evidence=0)
                   if r.reviewer_id == 'reviewer-a' else r for r in self.ratings]
        result = aggregate_teacher_reviews(self.batch, ratings)
        self.assertEqual(result['status'], 'below_threshold')
        self.assertEqual(result['mean_score'], 4)
        self.assertEqual(len(result['disagreements']), 3)
        self.assertEqual(result['disagreements'][0]['score_difference'], 8)

    def test_duplicate_unknown_stale_card_or_report_rejected(self):
        invalid = (self.ratings + [self.ratings[0]],
                   [replace(self.ratings[0], sample_id='unknown')],
                   [replace(self.ratings[0], report_sha256='a'*64)],
                   [replace(self.ratings[0], card_sha256='b'*64)],
                   [replace(self.ratings[0], reviewer_id='reviewer-c')] + self.ratings)
        for ratings in invalid:
            with self.assertRaises(ValueError):
                aggregate_teacher_reviews(self.batch, ratings)

    def test_scores_are_exact_integers_and_serious_categories_fixed(self):
        for change in (dict(correctness=True), dict(evidence=-1), dict(progression=3),
                       dict(actionability=1.0), dict(severe_issues=('secret free text',)),
                       dict(reviewer_id='real name')):
            with self.assertRaises(ValueError):
                replace(self.ratings[0], **change)

    def test_exact_six_score_meets_threshold(self):
        ratings = [replace(r, correctness=1, evidence=1) for r in self.ratings]
        result = aggregate_teacher_reviews(self.batch, ratings)
        self.assertEqual(result['mean_score'], 6)
        self.assertTrue(result['threshold_met'])

    def test_unpublishable_sample_prevents_review_completion(self):
        batch = run_model_evaluation((case(evidence=replace(case().evidence, complete=False)),),
                                     Provider(), suite_version='demo-v1', mode='mock')
        result = aggregate_teacher_reviews(batch, [])
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['unreviewable_samples'], 3)

    def test_replaced_memory_card_cannot_reuse_original_report_ratings(self):
        first = self.batch.review_samples[0]
        stale = replace(first, card=replace(first.card, hint='different card'))
        batch = replace(self.batch, review_samples=(stale,) + self.batch.review_samples[1:])
        with self.assertRaises(ValueError):
            aggregate_teacher_reviews(batch, self.ratings)
