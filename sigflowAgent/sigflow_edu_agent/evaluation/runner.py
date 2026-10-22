"""Explicit operator evaluation/export/review entry; no automatic model scoring."""
import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import random

from ..deepseek import DeepSeekProvider
from ..domain import Card, CourseReference, CourseSpan, ProviderError
from .frozen import exact, load_frozen_model_suite, read_json, text
from .model import EvaluationBatch, ReviewSample, _encode, _hash, run_model_evaluation
from .review import TeacherRating, aggregate_teacher_reviews


def _write(path, value):
    encoded = json.dumps(value, ensure_ascii=True, indent=2, allow_nan=False)+'\n'
    if len(encoded.encode('utf-8')) > 16*1024*1024:
        raise ValueError('evaluation_artifact_too_large')
    with path.open('x', encoding='utf-8') as stream:
        stream.write(encoded)


def _public_card(card):
    value = asdict(card)
    value.pop('producer')
    return value


def _card(value):
    value = exact(value, Card)
    if type(value['level']) is not int or value['level'] not in (1, 2, 3):
        raise ValueError('invalid_review_batch')
    for key in ('hint', 'question'):
        text(value[key], 4800)
    if value['producer'] not in ('model', 'rule'):
        raise ValueError('invalid_review_batch')
    for key in ('evidence_ids', 'limitations', 'course_refs'):
        if type(value[key]) is not list or len(value[key]) > 16:
            raise ValueError('invalid_review_batch')
    for entry in value['evidence_ids']+value['limitations']:
        text(entry, 8192)
    references = []
    for row in value['course_refs']:
        row = exact(row, CourseReference)
        if type(row['source_spans']) is not list or type(row['source_urls']) is not list:
            raise ValueError('invalid_review_batch')
        row['source_spans'] = tuple(CourseSpan(**exact(span, CourseSpan)) for span in row['source_spans'])
        row['source_urls'] = tuple(row['source_urls'])
        references.append(CourseReference(**row))
    value.update(evidence_ids=tuple(value['evidence_ids']), limitations=tuple(value['limitations']),
                 course_refs=tuple(references))
    return Card(**value)


def load_review_batch(path, *, expected_report_sha256):
    _, value = read_json(path, 16*1024*1024)
    if (type(value) is not dict or set(value) != {'schema_version', 'report_sha256', 'report', 'samples'}
            or value['schema_version'] != 'edu.review-batch.v1' or type(value['report']) is not dict
            or value['report_sha256'] != expected_report_sha256
            or hashlib.sha256(_encode(value['report']).encode()).hexdigest() != expected_report_sha256
            or type(value['samples']) is not list or len(value['samples']) > 2560):
        raise ValueError('invalid_review_batch')
    expected = {row['sample_id']: row for row in value['report']['results']}
    samples, seen = [], set()
    for row in value['samples']:
        row = exact(row, ReviewSample)
        row['card'] = _card(row['card'])
        sample = ReviewSample(**row)
        original = expected.get(sample.sample_id)
        if (original is None or sample.sample_id in seen
                or _hash(asdict(sample.card)) != original['output_sha256']
                or any(getattr(sample, key) != original[key] for key in
                       ('case_id', 'repetition', 'evidence_sha256', 'evidence_origin'))):
            raise ValueError('review_binding_mismatch')
        seen.add(sample.sample_id)
        samples.append(sample)
    if seen != {key for key, row in expected.items() if row['output_sha256'] is not None}:
        raise ValueError('review_binding_mismatch')
    return EvaluationBatch(_encode(value['report']), tuple(samples))


def _run(args):
    suite = load_frozen_model_suite(args.manifest, args.cases, expected_manifest_sha256=args.manifest_sha256)
    # Reserve the caller's new directory before any model call. Existing output
    # must never cause an expensive batch to be repeated or overwritten.
    args.output_dir.mkdir(parents=True, exist_ok=False)
    provider = DeepSeekProvider.from_settings() if args.mode == 'deepseek' else None
    batch = run_model_evaluation(suite.cases, provider, suite_version=suite.suite_version,
        mode=args.mode, repeats=args.repeats, max_model_calls=args.max_model_calls,
        runtime_version=args.runtime_version, rag_version=args.rag_version)
    report = batch.report
    report['frozen_input'] = dict(manifest_sha256=suite.manifest_sha256, file_sha256=suite.suite_sha256,
        reviewers=list(suite.reviewers), review_authenticity='external_signoff_required',
        team_acceptance='not_determined')
    batch = EvaluationBatch(_encode(report), batch.review_samples)
    record = dict(schema_version='edu.review-batch.v1', report_sha256=batch.report_sha256,
                  report=report, samples=[asdict(sample) for sample in batch.review_samples])
    cases = {case.case_id: case for case in suite.cases}
    samples, ratings = [], []
    for sample in batch.review_samples:
        card = _public_card(sample.card)
        case = cases[sample.case_id]
        samples.append(dict(sample_id=sample.sample_id, question=case.question,
            evidence_sha256=sample.evidence_sha256, card_sha256=_hash(card),
            evidence=asdict(case.evidence), card=card))
        for reviewer in suite.reviewers:
            ratings.append(dict(report_sha256=batch.report_sha256, sample_id=sample.sample_id,
                card_sha256=_hash(card), reviewer_id=reviewer,
                correctness=None, progression=None, actionability=None, evidence=None, severe_issues=[]))
    random.SystemRandom().shuffle(samples)
    packet = dict(schema_version='edu.blind-review.v1', report_sha256=batch.report_sha256,
        dimensions=['correctness', 'progression', 'actionability', 'evidence'],
        score_range=[0, 2], samples=samples)
    if provider is not None and provider.config.api_key in _encode([record, packet]):
        raise ValueError('sensitive_review_output')
    _write(args.output_dir/'batch.json', record)
    _write(args.output_dir/'review-packet.json', packet)
    _write(args.output_dir/'ratings-template.json', ratings)
    for reviewer in suite.reviewers:
        _write(args.output_dir/('ratings-'+reviewer+'.json'), [r for r in ratings if r['reviewer_id'] == reviewer])
    print(json.dumps(dict(status='batch_written', report_sha256=batch.report_sha256,
        execution_kind=report['execution_kind'], model_calls=report['model_calls'],
        teacher_review='pending', product_acceptance='not_determined')))
    return int(bool(report['evidence_rejected'] or report['budget_exhausted']
                    or args.mode == 'deepseek' and report['model_passed'] != report['total']))


def _review(args):
    batch = load_review_batch(args.batch, expected_report_sha256=args.report_sha256)
    _, values = read_json(args.ratings, 8*1024*1024)
    if type(values) is not list or len(values) > 5120:
        raise ValueError('invalid_teacher_review')
    ratings = []
    samples = {sample.sample_id:sample for sample in batch.review_samples}
    for value in values:
        value = exact(value, TeacherRating)
        if type(value['severe_issues']) is not list:
            raise ValueError('invalid_teacher_rating')
        value['severe_issues'] = tuple(value['severe_issues'])
        if value['reviewer_id'] not in batch.report['frozen_input']['reviewers']:
            raise ValueError('unknown_teacher_rating')
        sample = samples.get(value['sample_id'])
        if sample is None or value['card_sha256'] != _hash(_public_card(sample.card)):
            raise ValueError('review_binding_mismatch')
        # Public ratings bind only the blinded card. After validation the
        # operator maps it back to the original, pinned internal card digest.
        value['card_sha256'] = _hash(asdict(sample.card))
        ratings.append(TeacherRating(**value))
    report = aggregate_teacher_reviews(batch, ratings)
    # Preserve existing review files too; a revised grading needs a new filename.
    _write(args.output, report)
    print(json.dumps(dict(status=report['status'], report_sha256=batch.report_sha256,
                         product_acceptance='not_determined')))
    return int(not report['threshold_met'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    run = commands.add_parser('run')
    run.add_argument('--manifest', required=True, type=Path)
    run.add_argument('--cases', required=True, type=Path)
    run.add_argument('--manifest-sha256', required=True)
    run.add_argument('--output-dir', required=True, type=Path)
    run.add_argument('--mode', choices=('rule', 'deepseek'), default='rule')
    run.add_argument('--repeats', type=int, choices=range(3, 11), default=3)
    run.add_argument('--max-model-calls', type=int, default=256)
    run.add_argument('--runtime-version', default='self-developed-v1')
    run.add_argument('--rag-version', default='not_used')
    review = commands.add_parser('review')
    review.add_argument('--batch', required=True, type=Path)
    review.add_argument('--report-sha256', required=True)
    review.add_argument('--ratings', required=True, type=Path)
    review.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        return _run(args) if args.command == 'run' else _review(args)
    except (ValueError, OSError, TypeError, KeyError, RecursionError, OverflowError, ProviderError):
        print('{"error":"invalid_evaluation_input_or_output"}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
