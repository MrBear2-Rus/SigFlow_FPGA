"""Metadata-only view of currently admitted course background, not card citations."""
from collections import Counter
from dataclasses import asdict


def course_state(loop):
    state = loop.state
    diagnostics = state.course_diagnostics
    retrieval = diagnostics.get('retrieval_status', state.course_status if state.course_status in (
        'disabled', 'unavailable', 'no_match') else 'ok')
    if 'coverage_status' in diagnostics:
        coverage = diagnostics['coverage_status']
    elif state.course_status in ('ok', 'partial', 'omitted'):
        coverage = 'catalogued'
    else:
        coverage = 'unknown'
    return dict(status=state.course_status, answerability='not_assessed',
        retrieval_status=retrieval, packing_status=state.course_status,
        coverage_status=coverage, review_status=diagnostics.get('review_status', 'not_assessed'),
        references=[asdict(ref) for ref in loop._course_refs],
        omitted=[asdict(item) for item in state.course_omitted],
        omission_counts=dict(Counter(item.reason for item in state.course_omitted)),
        missing_required_sources=list(diagnostics.get('missing_required_sources', ())),
        missing_required_evidence=list(diagnostics.get('missing_required_evidence', ())))


def quarantined_course(status):
    return dict(status=status, answerability='not_assessed', references=[], omitted=[],
                retrieval_status='not_checked', packing_status='not_checked',
        coverage_status='not_checked', review_status='not_assessed',
                omission_counts={}, missing_required_sources=[], missing_required_evidence=[])
