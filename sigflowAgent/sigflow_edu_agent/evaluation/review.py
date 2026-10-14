"""Aggregate submitted human ratings; never score with a model or certify deployment."""

from dataclasses import asdict, dataclass
import re

from .model import EvaluationBatch, _hash

SEVERE_CATEGORIES = {'hardware_error', 'answer_leak', 'fabricated_evidence', 'other_severe'}


@dataclass(frozen=True)
class TeacherRating:
    report_sha256: str
    sample_id: str
    card_sha256: str
    reviewer_id: str
    correctness: int
    progression: int
    actionability: int
    evidence: int
    severe_issues: tuple[str, ...] = ()

    def __post_init__(self):
        if (not all(type(v) is str and re.fullmatch(r'[a-f0-9]{64}', v) is not None
                    for v in (self.report_sha256, self.card_sha256))
                or type(self.sample_id) is not str or not 1 <= len(self.sample_id) <= 100
                or type(self.reviewer_id) is not str
                or re.fullmatch(r'reviewer-[a-z0-9-]{1,64}', self.reviewer_id) is None
                or not all(type(n) is int and 0 <= n <= 2 for n in
                           (self.correctness, self.progression, self.actionability, self.evidence))
                or type(self.severe_issues) is not tuple
                or not all(type(v) is str and v in SEVERE_CATEGORIES for v in self.severe_issues)
                or len(set(self.severe_issues)) != len(self.severe_issues)):
            raise ValueError('invalid_teacher_rating')

    @property
    def total(self):
        return self.correctness + self.progression + self.actionability + self.evidence


def aggregate_teacher_reviews(batch, ratings):
    """Require two reviewers per output and retain original dimensions.

    Host verifies reviewer identity/qualification and manages independent blind
    review. Repaired findings need a new batch and fresh ratings, not a boolean
    override of a serious issue in this aggregator.
    """
    if (not isinstance(batch, EvaluationBatch) or not isinstance(ratings, (tuple, list))
            or len(ratings) > 5120):
        raise ValueError('invalid_teacher_review')
    report = batch.report
    expected = {row['sample_id']: row for row in report['results']}
    reviewable = {sample.sample_id for sample in batch.review_samples}
    if len(reviewable) != len(batch.review_samples) or any(
            sample.sample_id not in expected or
            _hash(asdict(sample.card)) != expected[sample.sample_id]['output_sha256']
            for sample in batch.review_samples):
        raise ValueError('review_binding_mismatch')
    by_sample, reviewers, originals = {}, set(), []
    for rating in ratings:
        if not isinstance(rating, TeacherRating):
            raise ValueError('invalid_teacher_rating')
        rating.__post_init__()
        row = expected.get(rating.sample_id)
        if (row is None or rating.sample_id not in reviewable
                or rating.report_sha256 != batch.report_sha256 or rating.card_sha256 != row['output_sha256']):
            raise ValueError('review_binding_mismatch')
        selected = by_sample.setdefault(rating.sample_id, {})
        if rating.reviewer_id in selected:
            raise ValueError('duplicate_teacher_rating')
        selected[rating.reviewer_id] = rating
        reviewers.add(rating.reviewer_id)
        if len(reviewers) > 2:
            raise ValueError('two_reviewers_required')
        originals.append(asdict(rating))
    missing = []
    differences = []
    for sample_id in expected:
        selected = by_sample.get(sample_id, {})
        if len(selected) < 2:
            missing.append(dict(sample_id=sample_id, missing_count=2 - len(selected),
                                reviewable=sample_id in reviewable))
        else:
            first, second = (selected[key] for key in sorted(selected))
            dimensions = {key: abs(getattr(first, key) - getattr(second, key))
                          for key in ('correctness', 'progression', 'actionability', 'evidence')}
            if any(dimensions.values()):
                differences.append(dict(sample_id=sample_id, dimension_differences=dimensions,
                                        score_difference=abs(first.total - second.total)))
    severe = sum(len(r.severe_issues) for r in ratings)
    mean = sum(r.total for r in ratings) / len(ratings) if ratings else None
    if missing or not expected:
        status = 'incomplete'
    elif severe:
        status = 'needs_remediation'
    elif mean < 6:
        status = 'below_threshold'
    else:
        status = 'threshold_met'
    return dict(schema_version='edu.teacher-review.v1', report_sha256=batch.report_sha256,
                source_execution_kind=report['execution_kind'], product_acceptance='not_determined',
                status=status, threshold_met=status == 'threshold_met', mean_score=mean,
                sample_count=len(expected), reviewed_samples=sum(len(by_sample.get(s, {})) == 2 for s in expected),
                unreviewable_samples=len(set(expected) - reviewable), severe_issue_count=severe,
                reviewer_ids=sorted(reviewers), missing=missing, disagreements=differences, ratings=originals)
