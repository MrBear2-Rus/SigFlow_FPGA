"""Offline frozen-suite readiness; no settings, model calls, grading or acceptance."""
import argparse
import json

from ..domain import RunRequest
from ..patterns import assess
from ..pedagogy import valid_evidence
from .frozen import load_frozen_model_suite
from .model import _version


def _summary():
    # Never project case text, reviewer identifiers, paths or exception messages.
    return dict(schema_version='edu.evaluation-preflight.v1', status='blocked',
        can_run_real_batch=False, input_status='not_provided', issues=[],
        declarations=dict(suite_version_valid=False, reviewers_count=0),
        case_counts=dict(total=0, eligible=0, evidence_rejected=0, unsupported_pattern=0),
        origin_counts=dict(fixture=0, tool=0, rule=0),
        budget=dict(repeats=None, max_model_calls=None, required_first_pass=None,
            required_with_repair=None, first_pass_sufficient=False, with_repair_sufficient=False),
        budget_policy='first_pass_plus_at_most_one_repair_per_sample',
        real_model='not_run', real_tools='not_run', teacher_review='pending',
        review_authenticity='external_signoff_required', semantic_quality='not_scored',
        product_acceptance='not_determined')


def preflight(manifest_path=None, cases_path=None, *, expected_manifest_sha256=None,
              repeats=3, max_model_calls=256, mode='deepseek',
              runtime_version='self-developed-v1', rag_version='not_used'):
    """Return a sanitized fixed-schema report. A ready result is preparation only.

    The caller supplies the manifest digest through an independent trusted
    channel. Its declarations cannot authenticate teachers or dataset origins.
    All source labels survive unchanged, including synthetic fixture evidence.
    """
    report = _summary()
    if (type(repeats) is not int or not 3 <= repeats <= 10
            or type(max_model_calls) is not int or not 1 <= max_model_calls <= 512
            or not _version(runtime_version) or not _version(rag_version)):
        report.update(status='invalid_input', issues=['invalid_preflight_config'])
        return report
    budget = report['budget']
    budget.update(repeats=repeats, max_model_calls=max_model_calls)
    if manifest_path is None or cases_path is None or expected_manifest_sha256 is None:
        report['issues'] = ['frozen_model_suite_not_provided']
        return report
    try:
        suite = load_frozen_model_suite(manifest_path, cases_path,
            expected_manifest_sha256=expected_manifest_sha256)
    except (ValueError, OSError, TypeError, KeyError, RecursionError, OverflowError):
        report.update(status='invalid_input', input_status='invalid', issues=['invalid_frozen_model_suite'])
        return report
    report['input_status'] = 'loaded'
    report['declarations'].update(suite_version_valid=True, reviewers_count=len(suite.reviewers))
    counts, origins = report['case_counts'], report['origin_counts']
    counts['total'] = len(suite.cases)
    course_context = False
    for case in suite.cases:
        evidence = case.evidence
        origins[evidence.origin] += 1
        course_context = course_context or any(item.kind == 'course' for item in evidence.context_items)
        try:
            request = RunRequest(evidence.project_id, evidence.revision, case.case_id, case.question)
            valid = valid_evidence(evidence, request)
        except (TypeError, ValueError):
            valid = False
        if not valid:
            counts['evidence_rejected'] += 1
        elif assess(evidence).status != 'supported':
            counts['unsupported_pattern'] += 1
        else:
            counts['eligible'] += 1
    first_pass = counts['eligible'] * repeats
    with_repair = first_pass * 2
    budget.update(required_first_pass=first_pass, required_with_repair=with_repair,
        first_pass_sufficient=max_model_calls >= first_pass,
        with_repair_sufficient=max_model_calls >= with_repair)
    issues = report['issues']
    if counts['total'] < 40:
        issues.append('team_case_count_below_40')
    if counts['evidence_rejected']:
        issues.append('evidence_rejected')
    if counts['unsupported_pattern']:
        issues.append('unsupported_pattern')
    if mode != 'deepseek':
        issues.append('unsupported_execution_mode')
    if course_context and rag_version == 'not_used':
        issues.append('rag_version_required')
    if not budget['first_pass_sufficient']:
        issues.append('insufficient_first_pass_budget')
    if not budget['with_repair_sufficient']:
        issues.append('insufficient_repair_budget')
    if not issues:
        report.update(status='ready', can_run_real_batch=True)
    return report


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's normal error includes untrusted option values and paths.
        raise ValueError('invalid_preflight_arguments')


def main(argv=None):
    parser = _Parser(description=__doc__)
    parser.add_argument('--manifest')
    parser.add_argument('--cases')
    parser.add_argument('--manifest-sha256')
    parser.add_argument('--mode', default='deepseek')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--max-model-calls', type=int, default=256)
    parser.add_argument('--runtime-version', default='self-developed-v1')
    parser.add_argument('--rag-version', default='not_used')
    try:
        args = parser.parse_args(argv)
        report = preflight(args.manifest, args.cases, expected_manifest_sha256=args.manifest_sha256,
            mode=args.mode, repeats=args.repeats, max_model_calls=args.max_model_calls,
            runtime_version=args.runtime_version, rag_version=args.rag_version)
    except ValueError:
        report = _summary()
        report.update(status='invalid_input', issues=['invalid_preflight_arguments'])
    print(json.dumps(report, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False))
    return 2 if report['status'] == 'invalid_input' else int(not report['can_run_real_batch'])


if __name__ == '__main__':
    raise SystemExit(main())
